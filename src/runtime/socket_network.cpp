// socket_network.cpp — INetwork over one non-blocking UDP socket (ADR-0009),
// plus the EventFd wake primitive.

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/wire.hpp"

#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rudp_rt {

namespace {

sockaddr_in to_sockaddr(const Endpoint& ep) {
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = ep.ipv4_be;
  addr.sin_port = ep.port_be;
  return addr;
}

}  // namespace

SocketNetwork::SocketNetwork(const Options& opts) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd_ < 0) {
    throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
  }

  if (opts.so_rcvbuf > 0) {
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &opts.so_rcvbuf,
                 sizeof(opts.so_rcvbuf));
  }
  if (opts.so_sndbuf > 0) {
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &opts.so_sndbuf,
                 sizeof(opts.so_sndbuf));
  }
  socklen_t len = sizeof(granted_rcvbuf_);
  ::getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &granted_rcvbuf_, &len);
  len = sizeof(granted_sndbuf_);
  ::getsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &granted_sndbuf_, &len);
  // Startup validation: granted buffers must meet their configured minimum
  // or startup fails (spec §5; no silent clamping).
  if (opts.so_rcvbuf_min > 0 && granted_rcvbuf_ < opts.so_rcvbuf_min) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("SO_RCVBUF grant " +
                             std::to_string(granted_rcvbuf_) +
                             " below configured minimum " +
                             std::to_string(opts.so_rcvbuf_min));
  }
  if (opts.so_sndbuf_min > 0 && granted_sndbuf_ < opts.so_sndbuf_min) {
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error("SO_SNDBUF grant " +
                             std::to_string(granted_sndbuf_) +
                             " below configured minimum " +
                             std::to_string(opts.so_sndbuf_min));
  }

  if (opts.bind.ipv4_be != 0 || opts.bind.port_be != 0) {
    sockaddr_in addr = to_sockaddr(opts.bind);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      const int saved = errno;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error(std::string("bind: ") +
                               std::strerror(saved));
    }
  }

  if (opts.connect_peer && opts.peer) {
    sockaddr_in addr = to_sockaddr(*opts.peer);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      const int saved = errno;
      ::close(fd_);
      fd_ = -1;
      throw std::runtime_error(std::string("connect: ") +
                               std::strerror(saved));
    }
    connected_ = true;
  }

  sockaddr_in local{};
  socklen_t llen = sizeof(local);
  if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&local), &llen) == 0) {
    local_.ipv4_be = local.sin_addr.s_addr;
    local_.port_be = local.sin_port;
  }
}

SocketNetwork::~SocketNetwork() {
  if (fd_ >= 0) ::close(fd_);
}

SendResult SocketNetwork::send(const Endpoint& peer,
                               std::span<const std::byte> bytes) {
  while (true) {
    ssize_t n;
    if (connected_) {
      n = ::send(fd_, bytes.data(), bytes.size(), 0);
    } else {
      sockaddr_in addr = to_sockaddr(peer);
      n = ::sendto(fd_, bytes.data(), bytes.size(), 0,
                   reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    }
    if (n >= 0) return {SendStatus::SENT};
    switch (errno) {
      case EINTR:
        continue;  // transparent retry (ADR-0009)
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
        return {SendStatus::WOULD_BLOCK};
      case EMSGSIZE:
        return {SendStatus::MESSAGE_TOO_LARGE};
      case ENETUNREACH:
      case EHOSTUNREACH:
      case ENETDOWN:
        last_errno_ = errno;
        return {SendStatus::UNREACHABLE};
      case ECONNREFUSED:
        last_errno_ = errno;
        return {SendStatus::REFUSED};
      default:
        last_errno_ = errno;
        return {SendStatus::FATAL};
    }
  }
}

RecvResult SocketNetwork::receive(std::span<std::byte> buffer) {
  while (true) {
    sockaddr_in addr{};
    socklen_t alen = sizeof(addr);
    const ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), 0,
                                 reinterpret_cast<sockaddr*>(&addr), &alen);
    if (n >= 0) {
      RecvResult r;
      r.status = static_cast<std::size_t>(n) > kMaxDatagram
                     ? RecvStatus::TOO_LARGE
                     : RecvStatus::RECEIVED;
      r.peer.ipv4_be = addr.sin_addr.s_addr;
      r.peer.port_be = addr.sin_port;
      r.size = static_cast<std::uint32_t>(n);
      return r;
    }
    switch (errno) {
      case EINTR:
        continue;
      case EAGAIN:
#if EAGAIN != EWOULDBLOCK
      case EWOULDBLOCK:
#endif
        return {RecvStatus::WOULD_BLOCK, {}, 0};
      case ECONNREFUSED:
        // ICMP port-unreachable feedback on a connected socket; the caller
        // treats REFUSED-class receive errors as liveness information.
        last_errno_ = errno;
        return {RecvStatus::FATAL, {}, 0};
      default:
        last_errno_ = errno;
        return {RecvStatus::FATAL, {}, 0};
    }
  }
}

// ---------------------------------------------------------------------------
// EventFd
// ---------------------------------------------------------------------------

EventFd::EventFd() {
  fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd_ < 0) {
    throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
  }
}

EventFd::~EventFd() {
  if (fd_ >= 0) ::close(fd_);
}

void EventFd::notify() {
  const std::uint64_t one = 1;
  const ssize_t n = ::write(fd_, &one, sizeof(one));
  (void)n;  // EAGAIN simply means the counter is already non-zero
}

void EventFd::drain() {
  std::uint64_t value = 0;
  while (::read(fd_, &value, sizeof(value)) > 0) {
  }
}

}  // namespace rudp_rt
