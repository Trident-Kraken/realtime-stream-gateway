// net_faults.cpp — ADR-0009 socket-error mapping through a scripted fault
// INetwork adapter (the test anchor ADR-0009 promises): WOULD_BLOCK parks a
// datagram in the pending slot and the next pump retries it; a session-level
// send error tears the session down; the node keeps running.

#include "test_framework.hpp"

#include "rudp_rt/clock.hpp"
#include "rudp_rt/config.hpp"
#include "rudp_rt/session.hpp"
#include "rudp_rt/wire.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include <arpa/inet.h>

namespace rudp_test {

using namespace rudp_rt;

namespace {

// Scripted fault adapter: send results are consumed in script order; the
// last entry repeats. receive() never delivers (protocol input is driven
// directly through on_datagram).
struct ScriptedNetwork : INetwork {
  std::vector<SendStatus> script;
  std::size_t idx = 0;

  SendResult send(const Endpoint&, std::span<const std::byte>) override {
    const SendStatus status =
        script.empty() ? SendStatus::SENT
                       : script[std::min(idx, script.size() - 1)];
    if (idx < script.size()) ++idx;
    return {status};
  }

  RecvResult receive(std::span<std::byte>) override {
    return {RecvStatus::WOULD_BLOCK, {}, 0};
  }
};

Datagram join_ping(Endpoint peer, std::uint16_t stream_id) {
  Ping ping;
  ping.nonce = 7;
  ping.purpose = PingPurpose::JOIN;
  ping.client_epoch = 99;
  const std::size_t len = encoded_ping_size(ping.purpose);
  Datagram dg;
  dg.peer = peer;
  dg.bytes.assign(kHeaderSize + len, std::byte{0});
  Header h;
  h.version = kWireVersion;
  h.header_len = kHeaderLenWords;
  h.type = static_cast<std::uint8_t>(PacketType::PING);
  h.stream_id = stream_id;
  h.payload_len = static_cast<std::uint16_t>(len);
  encode_header(dg.bytes.data(), h);
  encode_ping(dg.bytes.data() + kHeaderSize, ping);
  return dg;
}

}  // namespace

RUDP_TEST(pending_datagram_parked_and_retried) {
  // ADR-0009: a WOULD_BLOCK send parks the datagram in the node's pending
  // slot; EPOLLOUT-driven retry emits the identical datagram later.
  VirtualClock clock;
  Config cfg;
  ScriptedNetwork net;
  net.script = {SendStatus::WOULD_BLOCK, SendStatus::SENT};
  ClientNode node(cfg, clock, Endpoint{htonl(0x0A000001), htons(40000)}, 1,
                  nullptr);
  node.start_join(ns_from_ms(10));

  const bool pending = node.pump(net, ns_from_ms(10));
  RUDP_CHECK(pending);  // EPOLLOUT stays armed
  RUDP_CHECK_EQ(node.counters_.send_would_block, 1u);
  RUDP_CHECK_EQ(node.control_depth(), 0u);  // popped from the control queue

  const bool still_pending = node.pump(net, ns_from_ms(20));
  RUDP_CHECK(!still_pending);  // parked JOIN went out on the retry
  RUDP_CHECK_EQ(node.counters_.send_would_block, 1u);
}

RUDP_TEST(session_send_error_tears_down_session) {
  // ADR-0009: UNREACHABLE on a session datagram is a bounded teardown
  // (SOCKET_ERROR): the session is marked dead with its queues dropped and
  // its generation bumped (INV-14); the node itself keeps running.
  VirtualClock clock;
  Config cfg;
  const Endpoint cl_ep{htonl(0x0A000002), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 42u; });

  node.on_datagram(join_ping(cl_ep, 1), ns_from_ms(5));
  RUDP_CHECK_EQ(node.session_count(), 1u);

  ScriptedNetwork net;
  net.script = {SendStatus::UNREACHABLE};
  (void)node.pump(net, ns_from_ms(5));  // session PONG send fails

  RUDP_CHECK_EQ(node.counters().send_errors, 1u);
  const auto sessions = node.collect_sessions();
  RUDP_CHECK_EQ(sessions.size(), 1u);  // reaped later by the session scan
  RUDP_CHECK_EQ(sessions[0].state, std::string("dead"));
  RUDP_CHECK(sessions[0].teardown_cause == TeardownCause::SOCKET_ERROR);
}

RUDP_TEST(control_rate_limit_evicts_stale_endpoints) {
  VirtualClock clock;
  Config cfg;
  GatewayNode node(cfg, clock, [] { return 42u; });

  // Flood 1024 distinct spoofed endpoints using unknown stream_id (2) so
  // rate_limits_ reaches kMaxTrackedEndpoints without creating sessions.
  for (std::uint32_t i = 0; i < 1024; ++i) {
    Endpoint ep{htonl(0x0A010000u + i), htons(30000)};
    node.on_datagram(join_ping(ep, 2), clock.now());
  }
  // Immediate 1025th endpoint within the same second is rate-dropped.
  const Endpoint fresh_ep{htonl(0x0A020001u), htons(30001)};
  node.on_datagram(join_ping(fresh_ep, 1), clock.now());
  RUDP_CHECK_EQ(node.counters().control_rate_dropped, 1u);
  RUDP_CHECK_EQ(node.session_count(), 0u);

  // After >= 1s, stale non-session entries are evicted so legitimate clients
  // can join without permanent lockout.
  clock.advance(kNsPerSec);
  node.on_datagram(join_ping(fresh_ep, 1), clock.now());
  RUDP_CHECK_EQ(node.session_count(), 1u);
}

RUDP_TEST(dead_session_does_not_block_max_sessions) {
  VirtualClock clock;
  Config cfg;
  cfg.max_sessions = 1;
  GatewayNode node(cfg, clock, [] { return 42u; });

  const Endpoint ep1{htonl(0x0A000001u), htons(40001)};
  node.on_datagram(join_ping(ep1, 1), ns_from_ms(5));
  RUDP_CHECK_EQ(node.session_count(), 1u);

  ScriptedNetwork net;
  net.script = {SendStatus::UNREACHABLE};
  (void)node.pump(net, ns_from_ms(5));  // ep1 session dies on send error

  // A new client ep2 can immediately join even though ep1's dead session record
  // is still retained in sessions_ for final telemetry collection.
  const Endpoint ep2{htonl(0x0A000002u), htons(40002)};
  node.on_datagram(join_ping(ep2, 1), ns_from_ms(10));
  const auto sessions = node.collect_sessions();
  RUDP_CHECK_EQ(sessions.size(), 2u);
  RUDP_CHECK_EQ(sessions[0].state, std::string("dead"));
  RUDP_CHECK_EQ(sessions[1].state, std::string("established"));
}

}  // namespace rudp_test
