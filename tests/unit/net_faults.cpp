// net_faults.cpp — ADR-0009 socket-error mapping through a scripted fault
// INetwork adapter (the test anchor ADR-0009 promises): WOULD_BLOCK parks a
// datagram in the pending slot and the next pump retries it; a session-level
// send error tears the session down; the node keeps running.

#include "test_framework.hpp"

#include "rudp_rt/clock.hpp"
#include "rudp_rt/config.hpp"
#include "rudp_rt/runtime.hpp"
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

struct RecordingNetwork : INetwork {
  std::vector<SendStatus> script;
  std::size_t idx = 0;
  std::size_t send_calls = 0;
  std::vector<Datagram> sent;

  SendResult send(const Endpoint& peer,
                  std::span<const std::byte> bytes) override {
    ++send_calls;
    const SendStatus status =
        script.empty() ? SendStatus::SENT
                       : script[std::min(idx, script.size() - 1)];
    if (idx < script.size()) ++idx;
    if (status == SendStatus::SENT) {
      Datagram dg;
      dg.peer = peer;
      dg.bytes.assign(bytes.begin(), bytes.end());
      sent.push_back(std::move(dg));
    }
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

RUDP_TEST(shutdown_drains_final_abandon_and_drops_data) {
  VirtualClock clock;
  Config cfg;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 42u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());

  RecordingNetwork net;
  (void)node.pump(net, clock.now());  // drain PONG OK
  RUDP_CHECK_EQ(net.sent.size(), 1u);
  net.sent.clear();

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = cfg.deadline_budget;
  frame.bytes.assign(12000, std::byte{0xA});
  node.offer_frame(std::move(frame), clock.now());

  // First DATA sends, second hits WOULD_BLOCK and parks in s.pending_out.
  net.script = {SendStatus::SENT, SendStatus::WOULD_BLOCK};
  net.idx = 0;
  RUDP_CHECK(node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.sent.size(), 1u);
  net.sent.clear();

  clock.advance(ns_from_ms(1));
  node.begin_shutdown(clock.now());
  RUDP_CHECK(!node.output_drained());

  net.script = {SendStatus::SENT};
  net.idx = 0;
  (void)node.pump(net, clock.now());

  std::size_t abandons = 0;
  std::size_t datas = 0;
  for (const auto& dg : net.sent) {
    auto dec = decode_header(dg.bytes);
    RUDP_CHECK(dec.reject == RejectReason::NONE);
    const auto type = static_cast<PacketType>(dec.header.type);
    if (type == PacketType::ABANDON) ++abandons;
    if (type == PacketType::DATA) ++datas;
  }
  RUDP_CHECK_GE(abandons, 1u);
  RUDP_CHECK_EQ(datas, 0u);  // parked DATA dropped on shutdown (INV-07)
  RUDP_CHECK(node.output_drained());

  // Shutdown pump hitting UNREACHABLE must still mark output drained cleanly.
  VirtualClock clock2;
  GatewayNode node2(cfg, clock2, [] { return 99u; });
  clock2.set(ns_from_ms(5));
  node2.on_datagram(join_ping(cl_ep, 1), clock2.now());
  RecordingNetwork drain_ok;
  (void)node2.pump(drain_ok, clock2.now());
  MediaFrame frame2;
  frame2.stream_id = 1;
  frame2.frame_id = 0;
  frame2.capture_time = clock2.now();
  frame2.deadline_budget = cfg.deadline_budget;
  frame2.bytes.assign(12000, std::byte{0xB});
  node2.offer_frame(std::move(frame2), clock2.now());
  clock2.advance(ns_from_ms(1));
  node2.begin_shutdown(clock2.now());
  RecordingNetwork fail_net;
  fail_net.script = {SendStatus::UNREACHABLE};
  (void)node2.pump(fail_net, clock2.now());
  RUDP_CHECK(node2.output_drained());
}

RUDP_TEST(abandoned_frame_purged_from_pending_and_data_queue) {
  VirtualClock clock;
  Config cfg;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 42u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());
  RecordingNetwork net;
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 1u);
  auto pong = decode_pong(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
  RUDP_CHECK(pong.reject == RejectReason::NONE);
  const std::uint32_t epoch = pong.pong.session_epoch;
  const std::uint32_t isn = pong.pong.server_isn;
  net.sent.clear();

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = ns_from_ms(50);
  frame.bytes.assign(12000, std::byte{0xA});
  node.offer_frame(std::move(frame), clock.now());

  // Packet 0 sends; packet 1 hits WOULD_BLOCK and parks in s.pending_out.
  net.script = {SendStatus::SENT, SendStatus::WOULD_BLOCK};
  net.idx = 0;
  RUDP_CHECK(node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.sent.size(), 1u);
  net.sent.clear();

  // Advance past initial SRTT (25ms) but before deadline (50ms); NACK packet 0
  // so its retransmission enters s.data_queue while packet 1 sits in pending_out.
  clock.advance(ns_from_ms(30));
  Nack nack;
  nack.session_epoch = epoch;
  nack.seqs = {isn};
  Datagram nack_dg;
  nack_dg.peer = cl_ep;
  const std::size_t nlen = encoded_nack_size(nack.seqs.size());
  nack_dg.bytes.assign(kHeaderSize + nlen, std::byte{0});
  Header nh;
  nh.version = kWireVersion;
  nh.header_len = kHeaderLenWords;
  nh.type = static_cast<std::uint8_t>(PacketType::NACK);
  nh.stream_id = 1;
  nh.payload_len = static_cast<std::uint16_t>(nlen);
  encode_header(nack_dg.bytes.data(), nh);
  encode_nack(nack_dg.bytes.data() + kHeaderSize, nack);
  node.on_datagram(nack_dg, clock.now());

  // Advance past the 50ms frame deadline so the sender abandons frame 0.
  clock.advance(ns_from_ms(30));
  node.on_timer(clock.now());

  net.script = {SendStatus::SENT};
  net.idx = 0;
  (void)node.pump(net, clock.now());

  std::size_t abandons = 0;
  std::size_t datas = 0;
  for (const auto& dg : net.sent) {
    auto dec = decode_header(dg.bytes);
    RUDP_CHECK(dec.reject == RejectReason::NONE);
    const auto type = static_cast<PacketType>(dec.header.type);
    if (type == PacketType::ABANDON) ++abandons;
    if (type == PacketType::DATA) ++datas;
  }
  RUDP_CHECK_GE(abandons, 1u);
  RUDP_CHECK_EQ(datas, 0u);  // INV-07: no DATA emitted after ABANDON
}

RUDP_TEST(session_data_queue_bounded_by_window) {
  VirtualClock clock;
  Config cfg;
  cfg.window_w = 16;
  cfg.max_frame_packets = 10;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 77u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());
  RecordingNetwork net;
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 1u);
  auto pong = decode_pong(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
  const std::uint32_t epoch = pong.pong.session_epoch;
  const std::uint32_t isn = pong.pong.server_isn;
  net.sent.clear();

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = ns_from_ms(5000);
  frame.bytes.assign(12000, std::byte{0xC});
  node.offer_frame(std::move(frame), clock.now());
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 10u);
  net.sent.clear();

  // Repeated NACK rounds without pumping must deduplicate/bound s.data_queue.
  Nack nack;
  nack.session_epoch = epoch;
  for (std::uint32_t i = 0; i < 10; ++i) nack.seqs.push_back(isn + i);
  Datagram nack_dg;
  nack_dg.peer = cl_ep;
  const std::size_t nlen = encoded_nack_size(nack.seqs.size());
  nack_dg.bytes.assign(kHeaderSize + nlen, std::byte{0});
  Header nh;
  nh.version = kWireVersion;
  nh.header_len = kHeaderLenWords;
  nh.type = static_cast<std::uint8_t>(PacketType::NACK);
  nh.stream_id = 1;
  nh.payload_len = static_cast<std::uint16_t>(nlen);
  encode_header(nack_dg.bytes.data(), nh);
  encode_nack(nack_dg.bytes.data() + kHeaderSize, nack);

  for (int round = 0; round < 4; ++round) {
    clock.advance(ns_from_ms(30));
    node.on_datagram(nack_dg, clock.now());
  }

  (void)node.pump(net, clock.now());
  RUDP_CHECK_LE(net.sent.size(), static_cast<std::size_t>(cfg.window_w));
  RUDP_CHECK_EQ(net.sent.size(), 10u);
}

RUDP_TEST(new_data_consumes_session_token_bucket) {
  VirtualClock clock;
  Config cfg;
  cfg.token_burst_bytes = 2480;         // exactly 2 max-size DATA datagrams
  cfg.token_rate_bytes_per_sec = 2480;  // 1 max datagram per 500ms
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 55u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());
  RecordingNetwork net;
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 1u);  // PONG OK
  net.sent.clear();

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = cfg.deadline_budget;
  frame.bytes.assign(12000, std::byte{0xD});  // 10 x 1240 B datagrams
  node.offer_frame(std::move(frame), clock.now());

  // At t0: burst allows exactly 2 datagrams.
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 2u);
  net.sent.clear();

  // At t0 (0 ns elapsed): bucket empty -> 0 additional datagrams.
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 0u);

  // Advance 500ms (+1240 B): exactly 1 more datagram allowed.
  clock.advance(ns_from_ms(500));
  (void)node.pump(net, clock.now());
  RUDP_CHECK_EQ(net.sent.size(), 1u);
}

RUDP_TEST(client_pump_preserves_pending_on_consecutive_would_block_and_no_token_spin) {
  VirtualClock clock;
  Config cfg;
  const Endpoint gw_ep{htonl(0x0A000001u), htons(40000)};
  ClientNode node(cfg, clock, gw_ep, 1, [] { return 11u; });

  clock.set(ns_from_ms(5));
  node.start_join(clock.now());  // queues PING #1
  clock.advance(ns_from_ms(200));
  node.on_timer(clock.now());    // queues PING #2

  RecordingNetwork net;
  net.script = {SendStatus::WOULD_BLOCK};
  RUDP_CHECK(node.pump(net, clock.now()));  // parks PING #1

  // Second consecutive WOULD_BLOCK must retry only PING #1 and must NOT pop
  // and overwrite it with PING #2.
  net.script = {SendStatus::WOULD_BLOCK, SendStatus::WOULD_BLOCK};
  net.idx = 0;
  net.send_calls = 0;
  RUDP_CHECK(node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.send_calls, 1u);

  // When socket clears, both PING #1 and PING #2 transmit cleanly.
  net.script = {SendStatus::SENT, SendStatus::SENT};
  net.idx = 0;
  RUDP_CHECK(!node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.sent.size(), 2u);

  // Token-bucket exhaustion must NOT return pending = true (avoids EPOLLOUT spin).
  Config tcfg;
  tcfg.control_reserve_bytes = 10;
  tcfg.token_rate_bytes_per_sec = 10;
  ClientNode tnode(tcfg, clock, gw_ep, 1, [] { return 22u; });
  tnode.start_join(clock.now());
  RecordingNetwork tnet;
  RUDP_CHECK(!tnode.pump(tnet, clock.now()));
  RUDP_CHECK_EQ(tnet.send_calls, 0u);
}

RUDP_TEST(gateway_node_control_preserves_pending_and_no_token_spin) {
  VirtualClock clock;
  Config cfg;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 33u; });

  clock.set(ns_from_ms(5));
  // Wrong stream_id routes REJECT_UNKNOWN_STREAM replies into node_control_.
  node.on_datagram(join_ping(cl_ep, 2), clock.now());
  node.on_datagram(join_ping(cl_ep, 2), clock.now());

  RecordingNetwork net;
  net.script = {SendStatus::WOULD_BLOCK};
  RUDP_CHECK(node.pump(net, clock.now()));

  // Consecutive WOULD_BLOCK must only attempt sending node_pending_ once.
  net.script = {SendStatus::WOULD_BLOCK, SendStatus::WOULD_BLOCK};
  net.idx = 0;
  net.send_calls = 0;
  RUDP_CHECK(node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.send_calls, 1u);

  net.script = {SendStatus::SENT, SendStatus::SENT};
  net.idx = 0;
  RUDP_CHECK(!node.pump(net, clock.now()));
  RUDP_CHECK_EQ(net.sent.size(), 2u);

  // Node control token exhaustion must NOT return pending = true.
  Config tcfg;
  tcfg.control_reserve_bytes = 10;
  tcfg.token_rate_bytes_per_sec = 10;
  GatewayNode tnode(tcfg, clock, [] { return 44u; });
  tnode.on_datagram(join_ping(cl_ep, 2), clock.now());
  RecordingNetwork tnet;
  RUDP_CHECK(!tnode.pump(tnet, clock.now()));
  RUDP_CHECK_EQ(tnet.send_calls, 0u);
}

RUDP_TEST(gateway_pump_session_enforces_turn_byte_limit) {
  VirtualClock clock;
  Config cfg;
  cfg.turn_byte_limit = 2500;
  cfg.turn_datagram_limit = 64;
  cfg.token_burst_bytes = 1 << 20;
  cfg.token_rate_bytes_per_sec = 1 << 20;
  cfg.max_frame_packets = 10;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 55u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());
  RecordingNetwork net;
  (void)node.pump(net, clock.now());
  auto pong = decode_pong(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
  const std::uint32_t epoch = pong.pong.session_epoch;
  const std::uint32_t isn = pong.pong.server_isn;
  net.sent.clear();

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = ns_from_ms(5000);
  frame.bytes.assign(12000, std::byte{0xE});  // 10 x 1240 B datagrams
  node.offer_frame(std::move(frame), clock.now());

  // Pump out all 10 initial DATA datagrams across turns.
  for (int i = 0; i < 6; ++i) {
    (void)node.pump(net, clock.now());
  }
  RUDP_CHECK_EQ(net.sent.size(), 10u);
  net.sent.clear();

  // NACK 5 packets into s.data_queue after SRTT; single pump must honor
  // turn_byte_limit (2500 B -> at most 2 x 1240 B = 2480 B).
  clock.advance(ns_from_ms(30));
  Nack nack;
  nack.session_epoch = epoch;
  for (std::uint32_t i = 0; i < 5; ++i) nack.seqs.push_back(isn + i);
  Datagram nack_dg;
  nack_dg.peer = cl_ep;
  const std::size_t nlen = encoded_nack_size(nack.seqs.size());
  nack_dg.bytes.assign(kHeaderSize + nlen, std::byte{0});
  Header nh;
  nh.version = kWireVersion;
  nh.header_len = kHeaderLenWords;
  nh.type = static_cast<std::uint8_t>(PacketType::NACK);
  nh.stream_id = 1;
  nh.payload_len = static_cast<std::uint16_t>(nlen);
  encode_header(nack_dg.bytes.data(), nh);
  encode_nack(nack_dg.bytes.data() + kHeaderSize, nack);
  node.on_datagram(nack_dg, clock.now());

  (void)node.pump(net, clock.now());
  std::size_t bytes_sent = 0;
  for (const auto& dg : net.sent) bytes_sent += dg.bytes.size();
  RUDP_CHECK_EQ(net.sent.size(), 2u);
  RUDP_CHECK_LE(bytes_sent, static_cast<std::size_t>(cfg.turn_byte_limit));
}

RUDP_TEST(gateway_idle_to_active_resets_ack_clock_and_invalid_ack_does_not) {
  VirtualClock clock;
  Config cfg;
  cfg.keepalive_max_probes = 20;  // isolate ACK-starvation timing from keepalive
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 77u; });

  clock.set(ns_from_ms(5));
  node.on_datagram(join_ping(cl_ep, 1), clock.now());
  RecordingNetwork net;
  (void)node.pump(net, clock.now());
  auto pong = decode_pong(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
  const std::uint32_t epoch = pong.pong.session_epoch;
  const std::uint32_t isn = pong.pong.server_isn;
  net.sent.clear();

  // Stay idle with empty window past 3 * rto_max (4000 ms > 3000 ms).
  clock.set(ns_from_ms(4000));
  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.capture_time = clock.now();
  frame.deadline_budget = ns_from_ms(5000);
  frame.bytes.assign(1000, std::byte{0x1});
  node.offer_frame(std::move(frame), clock.now());
  (void)node.pump(net, clock.now());

  // Idle -> active transition must not immediately fire watchdog or tear down.
  clock.set(ns_from_ms(4010));
  node.on_timer(clock.now());
  RUDP_CHECK_EQ(node.watchdog_violations(), 0u);
  RUDP_CHECK_EQ(node.collect_sessions()[0].state, std::string("established"));

  auto send_ack_dg = [&](std::uint32_t ack_epoch, std::uint32_t ack_num) {
    Ack a;
    a.session_epoch = ack_epoch;
    a.ack_number = ack_num;
    Datagram dg;
    dg.peer = cl_ep;
    dg.bytes.assign(kHeaderSize + encoded_ack_size(), std::byte{0});
    Header h;
    h.version = kWireVersion;
    h.header_len = kHeaderLenWords;
    h.type = static_cast<std::uint8_t>(PacketType::ACK);
    h.stream_id = 1;
    h.payload_len = static_cast<std::uint16_t>(encoded_ack_size());
    encode_header(dg.bytes.data(), h);
    encode_ack(dg.bytes.data() + kHeaderSize, a);
    node.on_datagram(dg, clock.now());
  };

  // Invalid ACKs (wrong epoch and out-of-range future seq) must NOT reset the
  // ACK starvation timer.
  clock.set(ns_from_ms(6500));
  send_ack_dg(epoch + 1u, isn);
  send_ack_dg(epoch, isn + 500u);

  // At 7100ms (> 4000ms + 3 * 1000ms rto_max), ACK starvation must tear down.
  clock.set(ns_from_ms(7100));
  node.on_timer(clock.now());
  const auto sessions = node.collect_sessions();
  RUDP_CHECK_EQ(sessions.size(), 1u);
  RUDP_CHECK_EQ(sessions[0].state, std::string("dead"));
  RUDP_CHECK(sessions[0].teardown_cause == TeardownCause::SESSION_TIMEOUT);
}

RUDP_TEST(client_keepalive_waits_after_final_probe_and_clears_stale_queue) {
  auto make_pong_dg = [](Endpoint ep, std::uint16_t stream_id,
                         std::uint32_t nonce, PongStatus status,
                         std::uint32_t epoch, std::uint32_t isn) {
    Pong p;
    p.nonce = nonce;
    p.status = status;
    p.session_epoch = epoch;
    p.server_isn = isn;
    Datagram dg;
    dg.peer = ep;
    dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
    Header h;
    h.version = kWireVersion;
    h.header_len = kHeaderLenWords;
    h.type = static_cast<std::uint8_t>(PacketType::PONG);
    h.stream_id = stream_id;
    h.payload_len = static_cast<std::uint16_t>(encoded_pong_size());
    encode_header(dg.bytes.data(), h);
    encode_pong(dg.bytes.data() + kHeaderSize, p);
    return dg;
  };

  // Part A: probe #3 answered inside keepalive_min_interval keeps session alive.
  {
    VirtualClock clock;
    Config cfg;  // silence=200ms, min_interval=500ms, max_probes=3
    const Endpoint gw_ep{htonl(0x0A000001u), htons(40000)};
    ClientNode client(cfg, clock, gw_ep, 1, [] { return 11u; });
    clock.set(0);
    client.start_join(0);
    RecordingNetwork net;
    (void)client.pump(net, 0);
    auto jping = decode_ping(
        std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
    client.on_datagram(
        make_pong_dg(gw_ep, 1, jping.ping.nonce, PongStatus::OK, 55, 1000), 0);
    RUDP_CHECK(client.established());
    net.sent.clear();

    for (MonoNs t : {ns_from_ms(500), ns_from_ms(1000), ns_from_ms(1500)}) {
      clock.set(t);
      client.on_timer(t);
      (void)client.pump(net, t);
    }
    // Must still be established right after sending probe #3!
    RUDP_CHECK(client.established());
    auto p3 = decode_ping(
        std::span<const std::byte>(net.sent.back().bytes).subspan(kHeaderSize));
    clock.set(ns_from_ms(1600));
    client.on_datagram(
        make_pong_dg(gw_ep, 1, p3.ping.nonce, PongStatus::OK, 55, 1000),
        ns_from_ms(1600));
    clock.set(ns_from_ms(2000));
    client.on_timer(ns_from_ms(2000));
    RUDP_CHECK(client.established());
  }

  // Part B: no answer after probe #3 tears down at +500ms, purges stale
  // queued old-epoch KEEPALIVE PINGs, and schedules clean rejoin from `now`.
  {
    VirtualClock clock;
    Config cfg;
    const Endpoint gw_ep{htonl(0x0A000001u), htons(40000)};
    ClientNode client(cfg, clock, gw_ep, 1, [] { return 22u; });
    clock.set(0);
    client.start_join(0);
    RecordingNetwork net;
    (void)client.pump(net, 0);
    auto jping = decode_ping(
        std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
    client.on_datagram(
        make_pong_dg(gw_ep, 1, jping.ping.nonce, PongStatus::OK, 66, 2000), 0);
    net.sent.clear();

    // Queue all 3 keepalive probes into control_ without pumping them yet.
    for (MonoNs t : {ns_from_ms(500), ns_from_ms(1000), ns_from_ms(1500)}) {
      clock.set(t);
      client.on_timer(t);
    }
    RUDP_CHECK(client.established());
    RUDP_CHECK_EQ(client.control_depth(), 3u);

    clock.set(ns_from_ms(2000));
    client.on_timer(ns_from_ms(2000));
    RUDP_CHECK(!client.established());
    RUDP_CHECK(client.joining());
    RUDP_CHECK_EQ(client.control_depth(), 1u);  // only the fresh JOIN PING
    (void)client.pump(net, ns_from_ms(2000));
    RUDP_CHECK_EQ(net.sent.size(), 1u);
    auto fresh_join = decode_ping(
        std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
    RUDP_CHECK(fresh_join.reject == RejectReason::NONE);
    RUDP_CHECK(fresh_join.ping.purpose == PingPurpose::JOIN);
  }
}

RUDP_TEST(client_validates_ping_pong_stream_purpose_epoch_and_status) {
  VirtualClock clock;
  Config cfg;
  const Endpoint gw_ep{htonl(0x0A000001u), htons(40000)};
  ClientNode client(cfg, clock, gw_ep, 1, [] { return 33u; });
  clock.set(0);
  client.start_join(0);
  RecordingNetwork net;
  (void)client.pump(net, 0);
  auto jping = decode_ping(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));
  net.sent.clear();

  auto make_pong = [&](std::uint16_t stream_id, std::uint32_t nonce,
                       PongStatus st, std::uint32_t epoch) {
    Pong p;
    p.nonce = nonce;
    p.status = st;
    p.session_epoch = epoch;
    p.server_isn = 1000;
    Datagram dg;
    dg.peer = gw_ep;
    dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
    Header h;
    h.version = kWireVersion;
    h.header_len = kHeaderLenWords;
    h.type = static_cast<std::uint8_t>(PacketType::PONG);
    h.stream_id = stream_id;
    h.payload_len = static_cast<std::uint16_t>(encoded_pong_size());
    encode_header(dg.bytes.data(), h);
    encode_pong(dg.bytes.data() + kHeaderSize, p);
    return dg;
  };
  auto make_ping = [&](std::uint16_t stream_id, PingPurpose purpose,
                       std::uint32_t epoch) {
    Ping p;
    p.nonce = 999;
    p.purpose = purpose;
    p.session_epoch = epoch;
    p.client_epoch = 33;
    Datagram dg;
    dg.peer = gw_ep;
    const std::size_t plen = encoded_ping_size(purpose);
    dg.bytes.assign(kHeaderSize + plen, std::byte{0});
    Header h;
    h.version = kWireVersion;
    h.header_len = kHeaderLenWords;
    h.type = static_cast<std::uint8_t>(PacketType::PING);
    h.stream_id = stream_id;
    h.payload_len = static_cast<std::uint16_t>(plen);
    encode_header(dg.bytes.data(), h);
    encode_ping(dg.bytes.data() + kHeaderSize, p);
    return dg;
  };

  client.on_datagram(make_pong(1, jping.ping.nonce, PongStatus::OK, 42), 0);
  RUDP_CHECK(client.established());

  // Inbound PING validation: wrong stream_id, wrong purpose (JOIN), wrong epoch.
  client.on_datagram(make_ping(2, PingPurpose::KEEPALIVE, 42), ns_from_ms(10));
  RUDP_CHECK_EQ(client.counters_.stream_mismatch, 1u);
  RUDP_CHECK_EQ(client.control_depth(), 0u);

  client.on_datagram(make_ping(1, PingPurpose::JOIN, 42), ns_from_ms(10));
  RUDP_CHECK_EQ(client.counters_.stale_control, 1u);
  RUDP_CHECK_EQ(client.control_depth(), 0u);

  client.on_datagram(make_ping(1, PingPurpose::KEEPALIVE, 99), ns_from_ms(10));
  RUDP_CHECK_EQ(client.counters_.stale_control, 2u);
  RUDP_CHECK_EQ(client.control_depth(), 0u);

  // Inbound PONG validation on active keepalive probe.
  clock.set(ns_from_ms(500));
  client.on_timer(ns_from_ms(500));
  (void)client.pump(net, ns_from_ms(500));
  auto kp1 = decode_ping(
      std::span<const std::byte>(net.sent.back().bytes).subspan(kHeaderSize));

  client.on_datagram(make_pong(2, kp1.ping.nonce, PongStatus::OK, 42),
                     ns_from_ms(600));
  RUDP_CHECK_EQ(client.counters_.stream_mismatch, 2u);

  client.on_datagram(make_pong(1, kp1.ping.nonce, PongStatus::REJECT_STALE, 42),
                     ns_from_ms(600));
  RUDP_CHECK_EQ(client.counters_.stale_control, 3u);

  // Because REJECT_STALE and wrong-stream PONG did NOT clear unanswered_probes_,
  // 2 more unanswered probes + 500ms still trigger timeout at t=2000ms.
  for (MonoNs t : {ns_from_ms(1000), ns_from_ms(1500), ns_from_ms(2000)}) {
    clock.set(t);
    client.on_timer(t);
  }
  RUDP_CHECK(!client.established());
}

RUDP_TEST(shutdown_barrier_rejects_new_join) {
  VirtualClock clock;
  Config cfg;
  const Endpoint cl_ep{htonl(0x0A000002u), htons(40001)};
  GatewayNode node(cfg, clock, [] { return 42u; });

  node.begin_shutdown(ns_from_ms(10));
  node.on_datagram(join_ping(cl_ep, 1), ns_from_ms(12));
  RUDP_CHECK_EQ(node.session_count(), 0u);
  RUDP_CHECK_EQ(node.counters().stale_control, 1u);
  RUDP_CHECK(node.output_drained());
}

RUDP_TEST(gateway_start_rolls_back_on_failure) {
  {
    Config bad_bind;
    bad_bind.bind_address = "999.999.999.999";
    Gateway gw(bad_bind);
    bool threw = false;
    try {
      gw.start();
    } catch (const std::exception&) {
      threw = true;
    }
    RUDP_CHECK(threw);
  }

  {
    Config cfg1;
    cfg1.bind_address = "127.0.0.1";
    cfg1.port = 50398;
    Gateway gw1(cfg1);
    gw1.set_metrics_path("/tmp/rudp_gw_rollback_1.jsonl");
    gw1.start();

    Config cfg2 = cfg1;  // collides on 127.0.0.1:50398
    Gateway gw2(cfg2);
    gw2.set_metrics_path("/tmp/rudp_gw_rollback_2.jsonl");
    bool threw = false;
    try {
      gw2.start();
    } catch (const std::exception&) {
      threw = true;
    }
    RUDP_CHECK(threw);
  }
  std::remove("/tmp/rudp_gw_rollback_1.jsonl");
  std::remove("/tmp/rudp_gw_rollback_2.jsonl");
}

}  // namespace rudp_test
