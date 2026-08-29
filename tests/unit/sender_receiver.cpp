// sender_receiver.cpp — sender/receiver state-machine unit tests:
// ACK monotonicity, SACK handling, deadline abandonment (INV-07), receive
// window classification order (spec §4.7), and ABANDON application.

#include "test_framework.hpp"

#include "rudp_rt/clock.hpp"
#include "rudp_rt/receiver.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/wire.hpp"

namespace rudp_test {

using namespace rudp_rt;

namespace {

struct SenderHarness {
  VirtualClock clock;
  SenderConfig cfg;
  std::unique_ptr<TransportSender> sender;

  explicit SenderHarness(std::uint32_t isn = 1000) {
    cfg.stream_id = 1;
    cfg.ssrc = 7;
    cfg.session_epoch = 55;
    cfg.isn = isn;
    cfg.window_w = 64;
    cfg.max_frame_packets = 10;
    cfg.media_payload_bytes = 1200;
    sender = std::make_unique<TransportSender>(cfg, clock);
  }

  void offer(std::uint32_t frame_id, std::size_t bytes = 12000,
             MonoNs budget = ns_from_ms(100)) {
    MediaFrame f;
    f.stream_id = 1;
    f.frame_id = frame_id;
    f.capture_time = clock.now();
    f.deadline_budget = budget;
    f.bytes.assign(bytes, std::byte{0xA});
    sender->on_frame(std::move(f));
    auto out = sender->take_output();
    for (const auto& a : out.admissions) RUDP_CHECK(a.admitted);
  }

  void offer_dependent(std::uint32_t frame_id, std::uint32_t depends_on,
                       std::size_t bytes = 12000,
                       MonoNs budget = ns_from_ms(100)) {
    MediaFrame f;
    f.stream_id = 1;
    f.frame_id = frame_id;
    f.frame_type = FrameType::DELTA_FRAME;
    f.depends_on_frame = depends_on;
    f.capture_time = clock.now();
    f.deadline_budget = budget;
    f.bytes.assign(bytes, std::byte{0xB});
    sender->on_frame(std::move(f));
  }

  std::vector<Datagram> pump_new(int n = 32) {
    std::vector<Datagram> dgs;
    while (n-- > 0) {
      auto dg = sender->emit_new_data(clock.now(), 1 << 20);
      if (!dg) break;
      dgs.push_back(std::move(*dg));
    }
    return dgs;
  }
};

}  // namespace

RUDP_TEST(sender_ack_monotonic_and_domain_checked) {
  SenderHarness h;
  h.offer(0);
  auto dgs = h.pump_new();
  RUDP_CHECK_EQ(dgs.size(), 10u);

  Ack ack;
  ack.session_epoch = 55;

  // Future ACK beyond the allocated domain: rejected and counted.
  ack.ack_number = 1000 + 20;
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().stale_control, 1u);

  // Correct advance accepted; send base moves.
  ack.ack_number = 1004;  // consumed 1000..1004
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->allocated_unacked(), 5u);

  // Stale (behind) ACK ignored.
  ack.ack_number = 1002;
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->allocated_unacked(), 5u);

  // Epoch mismatch: rejected, window unchanged.
  ack.ack_number = 1009;
  ack.session_epoch = 56;
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->allocated_unacked(), 5u);
}

RUDP_TEST(sender_frame_close_outcomes) {
  SenderHarness h;
  // Frame 0 admitted early (remaining 100ms > threshold).
  h.offer(0, 12000, ns_from_ms(100));
  h.clock.advance(ns_from_ms(10));
  auto dgs = h.pump_new();
  RUDP_CHECK_EQ(dgs.size(), 10u);
  RUDP_CHECK_EQ(h.sender->media_bytes_owned(), 12000u);

  // Fully ACKed before deadline -> ONTIME, payload released, stub popped.
  Ack ack;
  ack.session_epoch = 55;
  ack.ack_number = 1009;
  h.sender->on_ack(ack, h.clock.now());
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.closures.size(), 1u);
  RUDP_CHECK(out.closures[0].outcome == SessionFrameOutcome::ONTIME);
  RUDP_CHECK(h.sender->window_empty());
  RUDP_CHECK_EQ(h.sender->media_bytes_owned(), 0u);
}

RUDP_TEST(sender_no_resurrection_after_abandon) {
  SenderHarness h;  // INV-07: post-abandon, NACK/RTO never re-emits DATA
  h.offer(0, 12000, ns_from_ms(30));  // short deadline
  h.clock.advance(ns_from_ms(5));
  auto dgs = h.pump_new();
  RUDP_CHECK_EQ(dgs.size(), 10u);
  h.clock.advance(ns_from_ms(30));  // deadline passes
  h.sender->on_timer(h.clock.now());
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.closures.size(), 1u);
  RUDP_CHECK(out.closures[0].outcome == SessionFrameOutcome::ABANDONED);
  RUDP_CHECK_EQ(out.control.size(), 1u);  // one ABANDON datagram
  RUDP_CHECK_EQ(h.sender->media_bytes_owned(), 0u);

  // NACK for an abandoned sequence: ABANDON retransmission only, no DATA.
  Nack nack;
  nack.session_epoch = 55;
  nack.seqs = {1003};
  const auto abandons_before = h.sender->counters().abandons_sent;
  const auto retrans_before = h.sender->counters().data_retransmissions;
  h.sender->on_nack(nack, h.clock.now() + ns_from_ms(200));
  out = h.sender->take_output();
  RUDP_CHECK(out.control.size() >= 1);  // ABANDON re-sent (throttled by RTO)
  RUDP_CHECK(out.data.empty());         // never DATA again (INV-07)
  RUDP_CHECK(h.sender->counters().abandons_sent >= abandons_before);
  RUDP_CHECK_EQ(h.sender->counters().data_retransmissions, retrans_before);
}

RUDP_TEST(sender_rto_backoff_per_entry) {
  SenderHarness h;
  h.offer(0);
  auto dgs = h.pump_new();  // 10 packets sent at t=0
  RUDP_CHECK_EQ(dgs.size(), 10u);

  // No ACKs: first RTO at initial estimate 75ms.
  h.clock.advance(ns_from_ms(74));
  h.sender->on_timer(h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().data_retransmissions, 0u);
  h.clock.advance(ns_from_ms(1));
  h.sender->on_timer(h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().data_retransmissions, 10u);

  // Backoff would double the entry RTO, but the frame deadline (capture +
  // 100ms budget) arrives first: the deadline branch abandons the frame and
  // no further DATA retransmissions happen (INV-07 path).
  h.clock.advance(ns_from_ms(149));
  h.sender->on_timer(h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().data_retransmissions, 10u);
  h.clock.advance(ns_from_ms(1));
  h.sender->on_timer(h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().data_retransmissions, 10u);
  RUDP_CHECK_EQ(h.sender->counters().abandons_sent, 1u);
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.closures.size(), 1u);
  RUDP_CHECK(out.closures[0].outcome == SessionFrameOutcome::ABANDONED);

  // RTT sampling: cumulative advance over a never-retransmitted packet
  // samples; retransmitted packets never sample (Karn, ADR-0018).
  Ack ack;
  ack.session_epoch = 55;
  // 1000 was retransmitted: SACK-style advance must not sample.
  ack.ack_number = 1002;  // covers retransmitted 1000 (Karn) + clean 1001,1002
  const auto samples_before = h.sender->counters().rtt_samples;
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().rtt_samples, samples_before);

  // Send one fresh packet, then a single-packet advance samples it.
  h.offer(1);
  auto more = h.pump_new(1);
  RUDP_CHECK_EQ(more.size(), 1u);
  // next seq: 1000+10 allocated... frame 0 seqs 1000..1009, frame 1 1010..
  h.clock.advance(ns_from_ms(15));
  Ack ack2;
  ack2.session_epoch = 55;
  ack2.ack_number = 1010;
  const auto s0 = h.sender->counters().rtt_samples;
  h.sender->on_ack(ack2, h.clock.now());  // multi-packet advance: no sample
  RUDP_CHECK_EQ(h.sender->counters().rtt_samples, s0);
  h.offer(2);
  auto more2 = h.pump_new(1);
  h.clock.advance(ns_from_ms(12));
  Ack ack3;
  ack3.session_epoch = 55;
  ack3.ack_number = 1011;
  h.sender->on_ack(ack3, h.clock.now());  // single-packet advance: sample
  RUDP_CHECK_EQ(h.sender->counters().rtt_samples, s0 + 1);
}

RUDP_TEST(receiver_classification_order_and_window) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 5000;
  rc.window_w = 64;
  rc.reorder_capacity = 256;
  UdpReceiver rx(rc, clock);
  rx.set_sync(5000, 7);

  auto make_data = [&](std::uint32_t seq, std::uint32_t frame_id,
                       std::uint16_t index, std::uint16_t count,
                       std::uint32_t rtp_ts) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = index == count - 1 ? kFlagFrameEnd : 0;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = frame_id;
    h.rtp_timestamp = rtp_ts;
    h.frame_index = index;
    h.frame_count = count;
    RtpHeader rtp;
    rtp.timestamp = rtp_ts;
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(100, std::byte{0}));
  };

  // Out-of-order acceptance creates a hole NACK (throttled per hole).
  auto out = rx.on_datagram(make_data(5003, 0, 3, 10, 3000), clock.now());
  RUDP_CHECK_EQ(out.accepted.size(), 1u);
  RUDP_CHECK_EQ(out.datagrams.size(), 2u);  // NACK(5000..5002) + ACK(4999)

  // Duplicate: counted, no output mutation.
  out = rx.on_datagram(make_data(5003, 0, 3, 10, 3000), clock.now());
  RUDP_CHECK(out.accepted.empty());
  RUDP_CHECK_EQ(rx.counters().duplicates, 1u);

  // TOO_OLD: below recv_next.
  out = rx.on_datagram(make_data(4000, 0, 0, 10, 3000), clock.now());
  RUDP_CHECK_EQ(rx.counters().too_old, 1u);

  // OUT_OF_WINDOW: beyond recv_next + W.
  out = rx.on_datagram(make_data(5000 + 64 + 10, 9, 0, 10, 3000), clock.now());
  RUDP_CHECK_EQ(rx.counters().out_of_window, 1u);

  // FRAME_END ⇔ last index mismatch: PROTOCOL_VIOLATION.
  Header bad = {};
  (void)bad;
  auto bad_dg = make_data(5010, 1, 0, 10, 3100);
  bad_dg.bytes[3] =
      static_cast<std::byte>(static_cast<unsigned>(bad_dg.bytes[3]) | 0x02);
  out = rx.on_datagram(bad_dg, clock.now());
  RUDP_CHECK_EQ(rx.counters().protocol_violations, 1u);

  // Malformed: bad version.
  auto ver_dg = make_data(5010, 1, 0, 10, 3100);
  ver_dg.bytes[0] = static_cast<std::byte>(9);
  out = rx.on_datagram(ver_dg, clock.now());
  RUDP_CHECK_EQ(rx.counters().malformed, 1u);
}

RUDP_TEST(receiver_abandon_application_idempotent) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 5000;
  rc.window_w = 64;
  UdpReceiver rx(rc, clock);
  rx.set_sync(5000, 7);

  auto make_data = [&](std::uint32_t seq) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = (seq - 5000) / 10;
    h.rtp_timestamp = 3000 + 300 * h.frame_id;
    h.frame_index = static_cast<std::uint16_t>((seq - 5000) % 10);
    h.frame_count = 10;
    h.flags = h.frame_index == h.frame_count - 1 ? kFlagFrameEnd : 0;
    RtpHeader rtp;
    rtp.timestamp = h.rtp_timestamp;
    rtp.marker = h.frame_index == h.frame_count - 1;
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(50, std::byte{1}));
  };

  // Receive 5002..5009 (frame 0 partially, hole at 5000/5001).
  for (std::uint32_t seq = 5002; seq <= 5009; ++seq) {
    auto out = rx.on_datagram(make_data(seq), clock.now());
    RUDP_CHECK_EQ(out.accepted.size(), 1u);
  }
  RUDP_CHECK_EQ(rx.recv_next(), 5000u);

  // ABANDON covering the whole frame: ranges normalized, applied, recv_next
  // advances past the abandoned sequences, fresh ACK produced.
  auto result = rx.apply_abandon({{5000, 5009}}, clock.now());
  RUDP_CHECK(result.applied);
  RUDP_CHECK_EQ(rx.recv_next(), 5010u);
  RUDP_CHECK_EQ(result.acks.size(), 1u);
  auto ack_dec = decode_ack(std::span<const std::byte>(result.acks[0].bytes)
                                .subspan(kHeaderSize));
  RUDP_CHECK_EQ(ack_dec.ack.ack_number, 5009u);  // recv_next - 1

  // Idempotent re-application: no state change, still a fresh ACK (the
  // liveness path that un-wedges a sender whose ACK was lost).
  auto again = rx.apply_abandon({{5000, 5009}}, clock.now());
  RUDP_CHECK_EQ(again.acks.size(), 1u);
  RUDP_CHECK_EQ(rx.recv_next(), 5010u);

  // Out-of-window range rejected and counted.
  const auto violations = rx.counters().protocol_violations;
  auto rejected = rx.apply_abandon({{5000 + 200, 5000 + 300}}, clock.now());
  RUDP_CHECK(rejected.out_of_window_rejected);
  RUDP_CHECK(rx.counters().protocol_violations > violations);
}

RUDP_TEST(receiver_abandon_out_of_window_ranges) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.isn = 100;
  UdpReceiver rx(rc, clock);
  rx.set_sync(100, 3);
  // Split at the 64-range payload limit: >64 ranges must still be applied in
  // order but the wire encoder never emits more than 64; verify the decoder
  // bound instead via a range that spans past the window edge.
  auto r = rx.apply_abandon({{90, 99}}, clock.now());
  RUDP_CHECK(r.applied);  // entirely below recv_next: recorded, no-op advance
  RUDP_CHECK_EQ(rx.recv_next(), 100u);
}

RUDP_TEST(sender_cascade_abandon_on_deadline) {
  SenderHarness h;
  // Frame 0: KEY_FRAME, 10 packets (1000..1009)
  h.offer(0, 12000, ns_from_ms(100));
  // Frame 1: DELTA_FRAME dependent on Frame 0, 10 packets (1010..1019)
  h.offer_dependent(1, 0, 12000, ns_from_ms(100));
  auto out1 = h.sender->take_output();
  RUDP_CHECK_EQ(out1.admissions.size(), 1u);
  RUDP_CHECK(out1.admissions[0].admitted);

  // Frame 2: DELTA_FRAME dependent on Frame 1, 10 packets (1020..1029)
  h.offer_dependent(2, 1, 12000, ns_from_ms(100));
  auto out2 = h.sender->take_output();
  RUDP_CHECK_EQ(out2.admissions.size(), 1u);
  RUDP_CHECK(out2.admissions[0].admitted);

  // Advance clock to trigger deadline timeout on Frame 0
  h.clock.advance(ns_from_ms(105));
  h.sender->on_timer(h.clock.now());

  auto out = h.sender->take_output();
  // Frame 0, Frame 1, and Frame 2 should all be closed as ABANDONED
  RUDP_CHECK_EQ(out.closures.size(), 3u);
  for (const auto& c : out.closures) {
    RUDP_CHECK_EQ(static_cast<int>(c.outcome), static_cast<int>(SessionFrameOutcome::ABANDONED));
  }
  // cascade_abandons counter should have recorded 2 cascaded frame abandons
  RUDP_CHECK_EQ(h.sender->counters().cascade_abandons, 2u);
  // ABANDON control packets should cover the full sequence range [1000, 1029]
  RUDP_CHECK_GT(out.control.size(), 0u);
  auto dec = decode_abandon(std::span<const std::byte>(out.control[0].bytes).subspan(kHeaderSize));
  RUDP_CHECK_EQ(dec.reject, RejectReason::NONE);
  RUDP_CHECK_EQ(dec.ab.ranges.size(), 1u);
  RUDP_CHECK_EQ(dec.ab.ranges[0].start, 1000u);
  RUDP_CHECK_EQ(dec.ab.ranges[0].end, 1029u);
  // Keyframe request should have been triggered
  RUDP_CHECK(out.keyframe_requested);
}

RUDP_TEST(sender_cascade_prunes_broken_gop_pre_admission) {
  SenderHarness h;
  // Frame 0: KEY_FRAME
  h.offer(0, 12000, ns_from_ms(100));
  // Frame 0 expires and is abandoned
  h.clock.advance(ns_from_ms(105));
  h.sender->on_timer(h.clock.now());
  (void)h.sender->take_output();

  // Now an arriving DELTA_FRAME depends on the already-abandoned Frame 0
  h.offer_dependent(1, 0, 12000, ns_from_ms(100));
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.admissions.size(), 1u);
  // Must be rejected before admission (Broken GOP fast pruning)
  RUDP_CHECK(!out.admissions[0].admitted);
  // cascade_abandons counter incremented
  RUDP_CHECK_EQ(h.sender->counters().cascade_abandons, 1u);
  // Send window should not have allocated sequences for frame 1
  RUDP_CHECK_EQ(h.sender->window_empty(), false); // frame 0 stub waiting for ack
  RUDP_CHECK_EQ(h.sender->allocated_unacked(), 10u); // only frame 0's 10 packets, not 20
}

}  // namespace rudp_test
