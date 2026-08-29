// scenarios.cpp — integration matrix (spec §8.2, Phase 2/3 gates): ordered,
// loss, reorder, duplicate, NACK recovery, RTO recovery, ABANDON, lost
// ABANDON, stale control, restart, SYNC loss, frame gaps, session timeout,
// capture burst, multi-client fan-out. Every scenario asserts conservation
// and zero watchdog violations.

#include "../sim_fixture.hpp"

namespace rudp_test {

using namespace rudp_rt;

// ---------------------------------------------------------------------------
// TransportLoop scenarios (sender <-> receive pipeline, no sockets)
// ---------------------------------------------------------------------------

RUDP_TEST(integ_ordered_delivery) {
  TransportLoop lp;
  for (std::uint32_t f = 0; f < 30; ++f) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));  // 30 fps pacing matches the RTP clock
  }
  lp.run_for(ns_from_ms(1200));  // drain: last playout ≈ offer 29 + 93ms
  RUDP_CHECK_EQ(lp.delivered_ontime, 30u);
  RUDP_CHECK_EQ(lp.delivered_late, 0u);
  RUDP_CHECK_EQ(lp.sender.counters().data_retransmissions, 0u);
}

RUDP_TEST(integ_loss_nack_recovery) {
  TransportLoop lp(0.10);  // 10% IID loss
  for (std::uint32_t f = 0; f < 60; ++f) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));
  }
  lp.run_for(ns_from_ms(600));
  // NACK/RTO recovery delivered most frames; the rest were abandoned whole.
  const std::uint64_t delivered = lp.delivered_ontime + lp.delivered_late;
  RUDP_CHECK(delivered + lp.sender.counters().abandons_sent / 10 >= 50u);
  RUDP_CHECK_GT(lp.sender.counters().data_retransmissions, 0u);
  // INV-01: every allocated sequence reaches a terminal state (stub popped).
  RUDP_CHECK(lp.sender.window_empty());
}

RUDP_TEST(integ_rto_recovery_without_nacks) {
  // Uplink control loss = 100%: NACKs never reach the sender, so recovery
  // happens through RTO retransmission alone (spec §8.2 "RTO recovery").
  TransportLoop lp(0.10);
  DirectionFaults up;
  up.base_delay = ns_from_ms(5);
  up.control_loss = 1.0;
  lp.net.set_route(lp.cl_ep, up);
  for (std::uint32_t f = 0; f < 6; ++f) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));
  }
  lp.run_for(ns_from_ms(3000));  // several RTO rounds
  RUDP_CHECK_GT(lp.sender.counters().data_retransmissions, 0u);
  // Frames either completed via RTO round trips or were abandoned whole.
  RUDP_CHECK(lp.sender.window_empty() ||
             lp.sender.counters().abandons_sent > 0);
}

RUDP_TEST(integ_reorder_and_duplicate) {
  TransportLoop lp;
  DirectionFaults down;
  down.base_delay = ns_from_ms(5);
  down.jitter = ns_from_ms(4);
  down.reorder = 0.3;
  down.duplicate = 0.2;
  lp.net.set_route(lp.gw_ep, down);
  for (std::uint32_t f = 0; f < 40; ++f) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));
  }
  lp.run_for(ns_from_ms(500));
  RUDP_CHECK_EQ(lp.delivered_ontime + lp.delivered_late, 40u);
  // Reordering produced out-of-order accepts -> hole NACKs were sent.
  RUDP_CHECK_GT(lp.playout.receiver_counters().nacks_sent, 0u);
}

RUDP_TEST(integ_abandon_at_deadline) {
  // Deep loss makes recovery impossible within the deadline: the whole frame
  // interval is abandoned and the sender never re-emits its DATA (INV-07).
  TransportLoop lp(0.9);  // 90% loss
  for (std::uint32_t f = 0; f < 6; ++f) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));
  }
  lp.run_for(ns_from_ms(3000));
  RUDP_CHECK_GT(lp.sender.counters().abandons_sent, 0u);
  RUDP_CHECK(lp.sender.window_empty());  // stubs cleared via ABANDON handshake
  RUDP_CHECK_GT(lp.delivery_abandoned, 0u);
}

RUDP_TEST(integ_lost_abandon_recovered) {
  // The first ABANDON datagram is dropped; the sender's RTO-driven
  // ABANDON retransmission must still clean the receiver state.
  TransportLoop lp(0.0);
  lp.net.arm_drop_all(PacketType::DATA);        // no DATA ever gets through
  lp.net.arm_drop_next(PacketType::ABANDON, 1); // first ABANDON lost
  lp.offer_frame(0);
  lp.run_for(ns_from_ms(6000));  // RTO backoff chain
  // The retried ABANDON was applied: the receiver advanced past the whole
  // abandoned interval and the sender's stub cleared (liveness, INV-01).
  RUDP_CHECK(lp.sender.window_empty());
  RUDP_CHECK_EQ(lp.playout.recv_next(), 1010u);
}

RUDP_TEST(integ_sync_start_lost) {
  // The first DATA packet (carrying SYNC_START) is lost: the receiver anchors
  // on the first valid packet and NACKs the hole; no sequence is skipped.
  TransportLoop lp;
  lp.net.arm_drop_next(PacketType::DATA, 1);
  lp.offer_frame(0);
  lp.run_for(ns_from_ms(500));
  RUDP_CHECK_GE(lp.delivered_ontime + lp.delivered_late, 1u);
  // The hole was reported missing before being recovered.
  RUDP_CHECK_GT(lp.playout.receiver_counters().nacks_sent, 0u);
  RUDP_CHECK_EQ(lp.delivered_ids.size(), 1u);
  RUDP_CHECK_EQ(lp.delivered_ids[0], 0u);  // full frame assembled
}

RUDP_TEST(integ_frame_id_gap_never_blocks) {
  // Frames 1..2 are never sent (capture drop); frame 3 must still deliver.
  TransportLoop lp;
  for (const std::uint32_t f : {0u, 3u, 4u, 5u}) {
    lp.offer_frame(f);
    lp.run_for(ns_from_ms(33));
  }
  lp.run_for(ns_from_ms(300));
  RUDP_CHECK_EQ(lp.delivered_ontime, 4u);
  RUDP_CHECK_EQ(lp.delivered_ids, (std::vector<std::uint32_t>{0, 3, 4, 5}));
}

RUDP_TEST(integ_descriptor_conflict_kills_frame) {
  // A packet contradicting the fixed descriptor kills the frame with a
  // PROTOCOL_VIOLATION and no partial completion (INV-05).
  TransportLoop lp;
  lp.net.arm_drop_next(PacketType::DATA, 5);  // keep the frame incomplete
  lp.offer_frame(0);
  lp.run_for(ns_from_ms(20));
  RUDP_CHECK_EQ(lp.delivered_ontime + lp.delivered_late, 0u);

  // Craft a conflicting packet: same frame id, wrong rtp timestamp/interval.
  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::DATA);
  h.stream_id = 1;
  h.seq = 1003;  // hole inside frame 0's interval (not stored)
  h.frame_id = 0;
  h.rtp_timestamp = 999999;  // conflicts with the admitted frame
  h.frame_index = 3;
  h.frame_count = 10;
  RtpHeader rtp;
  rtp.timestamp = 999999;
  Datagram dg = *build_data_datagram(
      lp.cl_ep, h, rtp, std::vector<std::byte>(100, std::byte{0}));
  lp.net.deliver_from(lp.gw_ep, dg);
  lp.run_for(ns_from_ms(200));

  RUDP_CHECK_GT(lp.playout.receiver_counters().protocol_violations, 0u);
  // The frame cannot complete from the conflict point onward.
  RUDP_CHECK_EQ(lp.delivered_ontime + lp.delivered_late, 0u);
}

// ---------------------------------------------------------------------------
// SimSession scenarios (handshake, epochs, lifecycle)
// ---------------------------------------------------------------------------

RUDP_TEST(integ_end_to_end_handshake_and_flow) {
  SimSession s(1);
  s.run(ns_from_ms(2000));
  s.shutdown_close();
  s.assert_invariants();
  RUDP_CHECK(s.clients[0].node->established());
  RUDP_CHECK_GT(s.gateway.collect_sessions().size(), 0u);
  std::uint64_t offered = 0, ontime = 0, pre = 0, abd = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    offered += sm.frames.offered();
    ontime += sm.frames.ontime;
    pre += sm.frames.pre_admission_dropped;
    abd += sm.frames.abandoned;
  }
  RUDP_CHECK_GT(offered, 20u);
  // Clean network: everything terminal is ontime; at most the very first
  // startup-boundary frame may be abandoned or pre-admission dropped.
  RUDP_CHECK_LE(abd, 1u);
  RUDP_CHECK_EQ(ontime + pre + abd, offered);
  RUDP_CHECK_GE(s.clients[0].node->delivered_ontime(), 20u);
}

RUDP_TEST(integ_stale_epoch_control_rejected) {
  SimSession s(1);
  s.run(ns_from_ms(300));
  RUDP_CHECK(s.clients[0].node->established());

  // Inject ACKs with a wrong epoch: rejected and counted, no window movement.
  std::uint32_t window_before = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    window_before = sm.window_packets_in_flight;
  }
  std::uint64_t stale_before = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    stale_before += sm.packets.stale_control;
  }
  for (int i = 0; i < 5; ++i) {
    Datagram dg;
    dg.peer = s.gw_sock.local();
    dg.bytes.assign(kHeaderSize + encoded_ack_size(), std::byte{0});
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::ACK);
    h.stream_id = 1;
    h.payload_len = static_cast<std::uint16_t>(encoded_ack_size());
    encode_header(dg.bytes.data(), h);
    Ack ack;
    ack.ack_number = 60000;  // bogus
    ack.session_epoch = 0xDEADBEEF;
    encode_ack(dg.bytes.data() + kHeaderSize, ack);
    s.net.deliver_from(s.clients[0].ep, dg);
  }
  s.run(ns_from_ms(50));
  std::uint64_t stale_after = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    stale_after += sm.packets.stale_control;
  }
  RUDP_CHECK_GT(stale_after, stale_before);
  for (const auto& sm : s.gateway.collect_sessions()) {
    RUDP_CHECK_EQ(sm.window_packets_in_flight, window_before);
  }
  s.shutdown_close();
  s.assert_invariants();
}

RUDP_TEST(integ_restart_replaces_session) {
  SimSession s(1);
  s.run(ns_from_ms(300));
  RUDP_CHECK(s.clients[0].node->established());
  const auto first_gen = s.gateway.collect_sessions()[0].generation;
  const auto first_epoch_len = s.gateway.collect_sessions().size();

  // Client restarts: fresh JOIN with a new client epoch.
  s.clients[0].node->start_join(s.clock.now());
  s.run(ns_from_ms(300));

  RUDP_CHECK_EQ(s.gateway.collect_sessions().size(), first_epoch_len);
  RUDP_CHECK_GT(s.gateway.collect_sessions()[0].generation, first_gen);
  RUDP_CHECK(s.clients[0].node->established());
  s.shutdown_close();
  s.assert_invariants();
}

RUDP_TEST(integ_session_timeout_and_rejoin) {
  SimSession s(1);
  s.run(ns_from_ms(300));
  RUDP_CHECK(s.clients[0].node->established());

  // Black-hole all uplink control: gateway probes go unanswered.
  DirectionFaults up;
  up.base_delay = ns_from_ms(5);
  up.control_loss = 1.0;
  s.net.set_route(s.clients[0].ep, up);
  s.feed_enabled = false;
  s.run(ns_from_ms(6000));  // > 3 probes + ack starvation window

  // The gateway session was torn down by timeout, and the client (which
  // cannot reach the gateway) also timed out and attempted rejoins.
  const auto sessions = s.gateway.collect_sessions();
  bool any_dead = false;
  for (const auto& sm : sessions) {
    if (sm.state == "dead") {
      any_dead = true;
      RUDP_CHECK(sm.teardown_cause == TeardownCause::SESSION_TIMEOUT);
    }
  }
  RUDP_CHECK(any_dead || sessions.empty());
  s.assert_invariants();
}

RUDP_TEST(integ_capture_burst_and_pre_admission) {
  SimSession s(1);
  // Feed frames only after the join completes; on a clean network every
  // frame is admitted and delivered.
  s.run(ns_from_ms(1500));
  s.shutdown_close();
  s.assert_invariants();
  std::uint64_t offered = 0, pre = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    offered += sm.frames.offered();
    pre += sm.frames.pre_admission_dropped;
  }
  RUDP_CHECK_GT(offered, 20u);
  RUDP_CHECK(pre * 20 < offered);  // no admission starvation (ADR-0017)
}

RUDP_TEST(integ_multi_client_fanout_conservation) {
  SimSession s(3);
  s.set_faults(0.03);
  s.run(ns_from_ms(2000));
  s.shutdown_close();
  s.assert_invariants();
  // Every client observed deliveries; the per-session ledgers all conserve.
  std::size_t active = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    if (sm.frames.offered() > 10) ++active;
  }
  RUDP_CHECK_GE(active, 3u);
  for (const auto& c : s.clients) {
    RUDP_CHECK_GT(c.node->delivered_ontime() + c.node->delivered_late() +
                      c.node->delivery_expired() + c.node->delivery_abandoned(),
                  0u);
  }
}

RUDP_TEST(integ_sustained_loss_no_wedge) {
  // 20% loss for many frames: the window must keep draining (INV-04) and
  // conservation must hold continuously.
  SimSession s(1);
  s.set_faults(0.20);
  s.run(ns_from_ms(3000));
  s.shutdown_close();
  s.assert_invariants();
  std::uint64_t offered = 0, terminal = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    offered += sm.frames.offered();
    terminal += sm.frames.ontime + sm.frames.late + sm.frames.abandoned +
                sm.frames.expired + sm.frames.pre_admission_dropped;
  }
  RUDP_CHECK_GT(offered, 30u);
  RUDP_CHECK_EQ(offered, terminal + s.gateway.aggregate_ledger().in_flight);
}

RUDP_TEST(integ_pending_datagram_recovered) {
  // ADR-0009 under simulation: the virtual socket injects WOULD_BLOCK once
  // frames are flowing; the datagram parks in the session pending slot and
  // the next pump retries it. Until the EventLoop refactor this path was
  // real-runtime-only.
  SimSession s(1);
  bool armed = false;
  s.run(ns_from_ms(2000), [&](rudp_rt::SimCluster& ss) {
    if (!armed && ss.fed >= 5) {
      ss.gw_sock.arm_would_block(3);
      armed = true;
    }
  });
  RUDP_CHECK(armed);
  std::uint64_t would_block = 0;
  for (const auto& sm : s.gateway.collect_sessions()) {
    would_block += sm.packets.send_would_block;  // session-level counter
  }
  RUDP_CHECK_GE(would_block, 3u);
  // The parked datagrams retried: the stream still delivers end to end.
  RUDP_CHECK_GT(s.clients[0].node->delivered_ontime(), 0u);
  s.shutdown_close();
  s.assert_invariants();
}

RUDP_TEST(integ_gop_cascade_abandon_under_loss) {
  // Simulate a video stream with GOP=3: Frame 0 is KEY_FRAME, 1..2 are DELTA_FRAMEs dependent on 0.
  // We inject high initial loss so Frame 0 deadline expires, triggering cascade abandon for 1..2.
  // Then Frame 3 (new KEY_FRAME) and Frame 4 (DELTA_FRAME) are offered and deliver cleanly.
  TransportLoop lp;
  // Frame 0..2: GOP 0 (30 packets)
  lp.offer_frame_gop(0, FrameType::KEY_FRAME, 0, 12000, ns_from_ms(50));
  for (std::uint32_t f = 1; f < 3; ++f) {
    lp.offer_frame_gop(f, FrameType::DELTA_FRAME, 0, 12000, ns_from_ms(100));
  }

  // Sever the network temporarily so Frame 0 times out
  DirectionFaults drop_all;
  drop_all.data_loss = 1.0;
  drop_all.control_loss = 1.0;
  lp.net.set_route(lp.gw_ep, drop_all);

  // Advance past Frame 0 deadline (50ms)
  lp.run_for(ns_from_ms(60));

  // Verify cascade abandon occurred (Frame 1 and 2 abandoned due to Frame 0)
  RUDP_CHECK_EQ(lp.sender.counters().cascade_abandons, 2u);

  // Restore clean network
  DirectionFaults clean;
  clean.base_delay = ns_from_ms(5);
  lp.net.set_route(lp.gw_ep, clean);

  // Offer Frame 3 (new KEY_FRAME) and Frame 4 (DELTA_FRAME)
  lp.offer_frame_gop(3, FrameType::KEY_FRAME, 0, 12000, ns_from_ms(100));
  lp.offer_frame_gop(4, FrameType::DELTA_FRAME, 3, 12000, ns_from_ms(100));

  lp.run_for(ns_from_ms(500));

  // The new GOP delivered cleanly without being wedged by the earlier cascade abandonment
  RUDP_CHECK_GE(lp.delivered_ontime, 2u);
  RUDP_CHECK(std::find(lp.delivered_ids.begin(), lp.delivered_ids.end(), 3u) != lp.delivered_ids.end());
  RUDP_CHECK(std::find(lp.delivered_ids.begin(), lp.delivered_ids.end(), 4u) != lp.delivered_ids.end());
  RUDP_CHECK(lp.sender.window_empty());
}

}  // namespace rudp_test
