#pragma once
// rudp_rt/network.hpp — transport abstraction shared by real sockets and the
// virtual network (spec §5: "Virtual and real networks implement the same
// interface"). Results use the closed SendResult/RecvResult vocabulary of
// types.hpp; errno never crosses this boundary (ADR-0009).

#include "rudp_rt/types.hpp"

#include <span>

namespace rudp_rt {

class INetwork {
 public:
  virtual ~INetwork() = default;

  // Sends one datagram. Never blocks; see ADR-0009 for result semantics.
  virtual SendResult send(const Endpoint& peer, std::span<const std::byte> bytes) = 0;

  // Receives one datagram into the caller buffer. Buffer must be at least
  // kMaxDatagram + 1 bytes so oversize input is detectable (TOO_LARGE).
  virtual RecvResult receive(std::span<std::byte> buffer) = 0;
};

}  // namespace rudp_rt
