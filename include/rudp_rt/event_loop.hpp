#pragma once
// rudp_rt/event_loop.hpp — EventLoop: the reactor shell shared by the Gateway
// and Client runtimes (spec §3.2). It owns the epoll set, the timerfd, the
// signalfd, and the timer heap (ADR-0010), and drives one Delegate through
// the fixed stage order of every wake:
//
//   wait (epoll) -> due timers -> drain_input -> pump
//     -> EPOLLOUT follows pending output -> after_wake
//
// The Delegate owns policy (what to drain, which counters to bump, when the
// shutdown barrier is complete); the EventLoop owns the machinery. Two
// adapters exist — Gateway and Client — which is what makes this seam real.

#include "rudp_rt/clock.hpp"
#include "rudp_rt/network.hpp"
#include "rudp_rt/types.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

#include <sys/epoll.h>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Timer heap (ADR-0010)
// ---------------------------------------------------------------------------
enum class TimerKind : std::uint8_t {
  SHUTDOWN = 0,
  SESSION_TIMEOUT = 1,
  FRAME_DEADLINE = 2,   // covered by the per-session scan entry
  RTO = 3,              // covered by the per-session scan entry
  ABANDON_RETRY = 4,    // covered by the per-session scan entry
  KEEPALIVE = 5,        // covered by the per-session scan entry
  WATCHDOG = 6,
  SESSION_SCAN = 7,     // one entry per live session, min of its deadlines
};

struct TimerEntry {
  std::uint32_t generation = 0;
  std::uint32_t session_id = 0;
  TimerKind kind = TimerKind::SESSION_SCAN;
  MonoNs deadline = 0;
  std::uint64_t insertion_id = 0;
};

class TimerHeap {
 public:
  explicit TimerHeap(int capacity) : capacity_(capacity) {}

  // Returns false (and does not insert) when the heap is at capacity.
  bool push(TimerEntry e);
  // Pops the earliest entry with deadline <= now; nullopt when none is due.
  std::optional<TimerEntry> pop_due(MonoNs now);
  std::optional<TimerEntry> peek() const;
  std::size_t size() const { return heap_.size(); }
  int capacity() const { return capacity_; }
  std::uint64_t high_water() const { return hwm_; }
  // Drops entries whose generation does not match; used at session teardown.
  void invalidate_generation(std::uint32_t session_id, std::uint32_t gen);

 private:
  bool less(const TimerEntry& a, const TimerEntry& b) const;
  int capacity_;
  std::vector<TimerEntry> heap_;
  std::uint64_t insertion_counter_ = 0;
  std::uint64_t hwm_ = 0;
};

// Event-wake primitive: eventfd wrapper used by workers to wake the reactor.
class EventFd {
 public:
  EventFd();
  ~EventFd();
  EventFd(const EventFd&) = delete;
  EventFd& operator=(const EventFd&) = delete;
  void notify();       // write (idempotent counter)
  void drain();        // read back to zero
  int fd() const { return fd_; }

 private:
  int fd_ = -1;
};

// ---------------------------------------------------------------------------
// EventLoop
// ---------------------------------------------------------------------------
class EventLoop {
 public:
  // Creates the epoll set, the timerfd, and the signalfd (SIGINT/SIGTERM
  // blocked process-wide). `timer_capacity` bounds the timer heap
  // (ADR-0010/0014). Throws std::runtime_error on failure.
  explicit EventLoop(int timer_capacity = 4096);
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  // Policy hooks; called only from run()'s thread.
  struct Delegate {
    virtual ~Delegate() = default;
    // Drain all readable input (socket to EAGAIN, capture queue, ...).
    virtual void drain_input(MonoNs now) = 0;
    // One due timer entry (SESSION_SCAN in production).
    virtual void on_timer(const TimerEntry& e, MonoNs now) = 0;
    // Emission round; return true while output remains pending so EPOLLOUT
    // stays armed (ADR-0009 pending datagrams).
    virtual bool pump(INetwork& net, MonoNs now) = 0;
    // Housekeeping after each wake: scan re-arm, metrics pacing.
    virtual void after_wake(MonoNs now) = 0;
    // Called once when SIGINT/SIGTERM arrives, stop() fires, or the run
    // duration elapses.
    virtual void on_stop() {}
    // True when the loop may exit after a stop (shutdown barrier complete,
    // INV-17/INV-18); return true when there is no barrier.
    virtual bool finished() const = 0;
    // epoll_wait timeout for the next round (ms).
    virtual int wait_timeout_ms() const { return 20; }
    // EPOLLERR/EPOLLHUP on the watched socket. The delegate owns the policy:
    // count + clear SO_ERROR (unconnected gateway), or ignore (connected
    // client, where recv() consumes the error, ADR-0009).
    virtual void on_socket_error() {}
  };

  // Registration, before run().
  void watch_socket(INetwork& net, int fd);  // EPOLLIN; EPOLLOUT managed by run()
  void watch_stop_wake(EventFd& wake);  // stop() notifier from other threads
  void watch_fd(int fd, std::function<void()> on_readable);

  // Timer heap access (ADR-0010).
  bool push_timer(TimerEntry e);
  const TimerHeap& timers() const { return timers_; }

  // Drains the watched socket to EAGAIN (level-triggered, spec §3.2):
  // on_dg per datagram, on_too_large for each oversize datagram.
  void drain_socket(const std::function<void(Datagram&&)>& on_dg,
                    const std::function<void()>& on_too_large);

  // Runs until the delegate is finished after a stop (signal, stop wake, or
  // end_at elapsing). One call per EventLoop instance.
  void run(Delegate& delegate, IClock& clock, MonoNs end_at);

 private:
  void arm_timerfd(MonoNs now);

  int epoll_fd_ = -1;
  int timer_fd_ = -1;
  int signal_fd_ = -1;
  INetwork* net_ = nullptr;
  int net_fd_ = -1;
  EventFd* wake_ = nullptr;
  std::map<int, std::function<void()>> custom_fds_;
  TimerHeap timers_;
};

}  // namespace rudp_rt
