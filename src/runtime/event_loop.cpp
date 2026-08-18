// event_loop.cpp — the reactor shell: epoll/timerfd/signalfd machinery and
// the fixed wake-stage order, shared by the Gateway and Client runtimes.

#include "rudp_rt/event_loop.hpp"
#include "rudp_rt/wire.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include <cerrno>
#include <signal.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

namespace rudp_rt {

namespace {

constexpr int kEpollMaxEvents = 16;

int epoll_ctl_add(int epfd, int fd, std::uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
}

int epoll_ctl_mod(int epfd, int fd, std::uint32_t events) {
  epoll_event ev{};
  ev.events = events;
  ev.data.fd = fd;
  return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
}

}  // namespace

EventLoop::EventLoop(int timer_capacity) : timers_(timer_capacity) {
  epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    throw std::runtime_error(std::string("epoll_create1: ") +
                             std::strerror(errno));
  }
  timer_fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  if (timer_fd_ < 0) {
    throw std::runtime_error(std::string("timerfd_create: ") +
                             std::strerror(errno));
  }
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  if (::pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
    throw std::runtime_error("pthread_sigmask failed");
  }
  signal_fd_ = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signal_fd_ < 0) {
    throw std::runtime_error(std::string("signalfd: ") + std::strerror(errno));
  }
  if (epoll_ctl_add(epoll_fd_, timer_fd_, EPOLLIN) != 0 ||
      epoll_ctl_add(epoll_fd_, signal_fd_, EPOLLIN) != 0) {
    throw std::runtime_error(std::string("epoll_ctl: ") +
                             std::strerror(errno));
  }
}

EventLoop::~EventLoop() {
  if (timer_fd_ >= 0) ::close(timer_fd_);
  if (signal_fd_ >= 0) ::close(signal_fd_);
  if (epoll_fd_ >= 0) ::close(epoll_fd_);
}

void EventLoop::watch_socket(INetwork& net, int fd) {
  net_ = &net;
  net_fd_ = fd;
  if (epoll_ctl_add(epoll_fd_, net_fd_, EPOLLIN) != 0) {
    throw std::runtime_error(std::string("epoll_ctl: ") +
                             std::strerror(errno));
  }
}

void EventLoop::watch_stop_wake(EventFd& wake) {
  wake_ = &wake;
  if (epoll_ctl_add(epoll_fd_, wake.fd(), EPOLLIN) != 0) {
    throw std::runtime_error(std::string("epoll_ctl: ") +
                             std::strerror(errno));
  }
}

void EventLoop::watch_fd(int fd, std::function<void()> on_readable) {
  if (epoll_ctl_add(epoll_fd_, fd, EPOLLIN) != 0) {
    throw std::runtime_error(std::string("epoll_ctl: ") +
                             std::strerror(errno));
  }
  custom_fds_[fd] = std::move(on_readable);
}

bool EventLoop::push_timer(TimerEntry e) { return timers_.push(std::move(e)); }

void EventLoop::drain_socket(const std::function<void(Datagram&&)>& on_dg,
                             const std::function<void()>& on_too_large) {
  if (net_ == nullptr) return;
  std::vector<std::byte> buffer(kMaxDatagram + 1);
  while (true) {
    RecvResult r = net_->receive(buffer);
    if (r.status == RecvStatus::RECEIVED) {
      Datagram dg;
      dg.peer = r.peer;
      dg.bytes.assign(buffer.begin(),
                      buffer.begin() + static_cast<std::ptrdiff_t>(r.size));
      on_dg(std::move(dg));
      continue;
    }
    if (r.status == RecvStatus::TOO_LARGE) {
      on_too_large();
      continue;
    }
    break;  // WOULD_BLOCK / FATAL ends the drain
  }
}

void EventLoop::arm_timerfd(MonoNs now) {
  itimerspec its{};
  if (auto top = timers_.peek()) {
    MonoNs delta = top->deadline - now;
    if (delta < 0) delta = 0;
    its.it_value.tv_sec = static_cast<time_t>(delta / kNsPerSec);
    its.it_value.tv_nsec = static_cast<long>(delta % kNsPerSec);
  }
  // Empty heap: zero it_value disarms the timerfd entirely.
  ::timerfd_settime(timer_fd_, 0, &its, nullptr);
}

void EventLoop::run(Delegate& delegate, IClock& clock, MonoNs end_at) {
  bool stop_requested = false;
  bool stop_notified = false;
  bool epollout_armed = false;

  while (true) {
    const MonoNs now = clock.now();
    if (!stop_requested && end_at > 0 && now >= end_at) {
      stop_requested = true;
    }

    arm_timerfd(now);

    epoll_event events[kEpollMaxEvents];
    const int n =
        ::epoll_wait(epoll_fd_, events, kEpollMaxEvents, delegate.wait_timeout_ms());
    const MonoNs post = clock.now();

    for (int i = 0; i < n; ++i) {
      const int fd = events[i].data.fd;
      if (fd == signal_fd_) {
        signalfd_siginfo si{};
        while (::read(signal_fd_, &si, sizeof(si)) > 0) {
          stop_requested = true;  // SIGINT/SIGTERM: graceful (INV-18)
        }
      } else if (wake_ != nullptr && fd == wake_->fd()) {
        wake_->drain();
        stop_requested = true;  // stop() requested
      } else if (fd == timer_fd_) {
        std::uint64_t expirations = 0;
        while (::read(timer_fd_, &expirations, sizeof(expirations)) > 0) {
        }
        while (auto e = timers_.pop_due(post)) {
          delegate.on_timer(*e, post);
        }
      } else if (net_ != nullptr && fd == net_fd_) {
        if (events[i].events & (EPOLLERR | EPOLLHUP)) {
          delegate.on_socket_error();
        }
      } else {
        auto it = custom_fds_.find(fd);
        if (it != custom_fds_.end()) it->second();
      }
    }

    if (stop_requested && !stop_notified) {
      stop_notified = true;
      delegate.on_stop();
    }

    delegate.drain_input(post);
    if (net_ != nullptr) {
      const bool pending = delegate.pump(*net_, post);
      if (pending && !epollout_armed) {
        epoll_ctl_mod(epoll_fd_, net_fd_, EPOLLIN | EPOLLOUT);
        epollout_armed = true;
      } else if (!pending && epollout_armed) {
        epoll_ctl_mod(epoll_fd_, net_fd_, EPOLLIN);
        epollout_armed = false;
      }
    }
    delegate.after_wake(post);

    if (stop_requested && delegate.finished()) break;
  }
}

}  // namespace rudp_rt
