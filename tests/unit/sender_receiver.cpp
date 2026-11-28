// sender_receiver.cpp — sender/receiver state-machine unit tests:
// ACK monotonicity, SACK handling, deadline abandonment (INV-07), receive
// window classification order (spec §4.7), and ABANDON application.

#include "test_framework.hpp"

#include "rudp_rt/clock.hpp"
#include "rudp_rt/playout.hpp"
#include "rudp_rt/receiver.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/session.hpp"
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
  RUDP_CHECK(!h.sender->take_output().keyframe_requested);
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

RUDP_TEST(assembler_overflow_preserves_remaining_frames) {
  FrameAssembler asm4(4);
  auto make_pkt = [](std::uint32_t fid, std::uint16_t idx,
                     std::uint16_t count) {
    AcceptedPacket p;
    p.frame_id = fid;
    p.seq = fid * count + idx;
    p.rtp_timestamp = fid * 3000;
    p.stream_id = 1;
    p.frame_index = idx;
    p.frame_count = count;
    p.frame_end = (idx + 1 == count);
    p.media.assign(10, static_cast<std::byte>(fid & 0xFF));
    return p;
  };

  // Insert first fragment of frames 0..3 (fills capacity = 4), with 1 arriving
  // before 0 to also verify out-of-order serial ordering across frames.
  RUDP_CHECK(asm4.on_packet(make_pkt(1, 0, 2)).expired_frames.empty());
  RUDP_CHECK(asm4.on_packet(make_pkt(0, 0, 2)).expired_frames.empty());
  RUDP_CHECK(asm4.on_packet(make_pkt(2, 0, 2)).expired_frames.empty());
  RUDP_CHECK(asm4.on_packet(make_pkt(3, 0, 2)).expired_frames.empty());
  RUDP_CHECK_EQ(asm4.tracked_frames(), 4u);

  // Frame 4 arrives: oldest frame in serial order (frame 0) must be evicted,
  // while frames 1, 2, 3 remain intact and addressable.
  auto out4 = asm4.on_packet(make_pkt(4, 0, 2));
  RUDP_CHECK_EQ(out4.expired_frames.size(), 1u);
  RUDP_CHECK_EQ(out4.expired_frames[0], 0u);
  RUDP_CHECK_EQ(asm4.tracked_frames(), 4u);

  // Second fragment of frames 1, 2, 3, 4 must each complete its frame cleanly.
  for (std::uint32_t fid = 1; fid <= 4; ++fid) {
    RUDP_CHECK_EQ(static_cast<int>(asm4.validate_descriptor(make_pkt(fid, 1, 2))),
                  static_cast<int>(DataClassification::VALID));
    auto done = asm4.on_packet(make_pkt(fid, 1, 2));
    RUDP_CHECK_EQ(done.completed.size(), 1u);
    RUDP_CHECK_EQ(done.completed[0].frame_id, fid);
    RUDP_CHECK_EQ(done.completed[0].bytes.size(), 20u);
  }
  RUDP_CHECK_EQ(asm4.tracked_frames(), 0u);
}

RUDP_TEST(receiver_abandon_ranges_pruned_and_nack_deadline) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 1000;
  rc.window_w = 64;
  rc.nack_throttle_srtt = ns_from_ms(25);
  UdpReceiver rx(rc, clock);
  rx.set_sync(1000, 7);

  auto make_data = [](std::uint32_t seq) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = seq - 1000;
    h.rtp_timestamp = 3000 * (seq - 1000);
    h.frame_index = 0;
    h.frame_count = 1;
    h.flags = kFlagFrameEnd;
    RtpHeader rtp;
    rtp.timestamp = h.rtp_timestamp;
    rtp.marker = true;
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(32, std::byte{1}));
  };

  // Arrive seq 1002 out of order -> holes at 1000..1001.
  rx.on_datagram(make_data(1002), clock.now());
  RUDP_CHECK(rx.next_nack_deadline().has_value());
  RUDP_CHECK_EQ(*rx.next_nack_deadline(), ns_from_ms(25));

  // Partial ABANDON for 1001 only: range [1001, 1001] stays until 1000 arrives.
  rx.apply_abandon({{1001, 1001}}, clock.now());
  RUDP_CHECK_EQ(rx.recv_next(), 1000u);
  RUDP_CHECK_EQ(rx.abandon_range_count(), 1u);

  // Now ABANDON 1000: recv_next advances across 1000, 1001 (abandoned), and
  // 1002 (held) to 1003; consumed abandon ranges and hole state are pruned.
  rx.apply_abandon({{1000, 1000}}, clock.now());
  RUDP_CHECK_EQ(rx.recv_next(), 1003u);
  RUDP_CHECK_EQ(rx.abandon_range_count(), 0u);
  RUDP_CHECK(!rx.next_nack_deadline().has_value());
}

RUDP_TEST(receiver_dead_frame_tombstone_wraparound) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 5000;
  rc.window_w = 64;
  UdpReceiver rx(rc, clock);
  rx.set_sync(5000, 7);

  // Mark 255 frames dead before wrap-around [0xFFFFFF01 .. 0xFFFFFFFF],
  // then wrap to 0 and 1 (total 257 tombstones > cap 256).
  // FIFO eviction must evict 0xFFFFFF01, NOT numeric minimum 0.
  for (std::uint32_t i = 0; i < 257; ++i) {
    const std::uint32_t fid = 0xFFFFFF01u + i;
    rx.mark_frame_dead(fid);
  }
  RUDP_CHECK(!rx.frame_dead(0xFFFFFF01u));
  RUDP_CHECK(rx.frame_dead(0xFFFFFFFFu));
  RUDP_CHECK(rx.frame_dead(0u));
  RUDP_CHECK(rx.frame_dead(1u));
}

RUDP_TEST(assembler_64_packet_frame_completes) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 2000;
  rc.window_w = 64;
  rc.reorder_capacity = 256;
  UdpReceiver rx(rc, clock);
  FrameAssembler assembler(64);
  rx.set_descriptor_hooks(
      [&](const AcceptedPacket& p) { return assembler.validate_descriptor(p); },
      [&](std::uint32_t fid) { assembler.on_frame_dead(fid); });
  rx.set_sync(2000, 7);

  auto make_pkt = [&](std::uint16_t idx) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = idx == 63 ? kFlagFrameEnd : 0;
    h.stream_id = 1;
    h.seq = 2000u + idx;
    h.frame_id = 10;
    h.rtp_timestamp = 30000;
    h.frame_index = idx;
    h.frame_count = 64;
    RtpHeader rtp;
    rtp.timestamp = 30000;
    rtp.marker = (idx == 63);
    return *build_data_datagram(
        Endpoint{1, 1}, h, rtp,
        std::vector<std::byte>(16, static_cast<std::byte>(idx)));
  };

  std::vector<CompletedFrame> completed;
  // Send index 32 first to verify bit 32 does not collide with bit 0.
  auto out32 = rx.on_datagram(make_pkt(32), clock.now());
  RUDP_CHECK_EQ(out32.accepted.size(), 1u);
  AcceptedPacket dup32 = out32.accepted[0];
  auto aout32 = assembler.on_packet(std::move(out32.accepted[0]));
  RUDP_CHECK(aout32.completed.empty());
  // Defensive duplicate at assembler level for bit 32 must be ignored cleanly.
  auto aout32_dup = assembler.on_packet(std::move(dup32));
  RUDP_CHECK(aout32_dup.completed.empty());

  for (std::uint16_t idx = 0; idx < 64; ++idx) {
    if (idx == 32) continue;
    auto out = rx.on_datagram(make_pkt(idx), clock.now());
    RUDP_CHECK_EQ(out.accepted.size(), 1u);
    for (auto& ap : out.accepted) {
      auto aout = assembler.on_packet(std::move(ap));
      completed.insert(completed.end(),
                       std::make_move_iterator(aout.completed.begin()),
                       std::make_move_iterator(aout.completed.end()));
    }
  }
  RUDP_CHECK_EQ(completed.size(), 1u);
  RUDP_CHECK_EQ(completed[0].frame_id, 10u);
  RUDP_CHECK_EQ(completed[0].bytes.size(), 64u * 16u);
  RUDP_CHECK_EQ(rx.recv_next(), 2064u);
}

RUDP_TEST(receiver_rejects_oversized_frame_count) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 2000;
  rc.window_w = 64;
  UdpReceiver rx(rc, clock);
  rx.set_sync(2000, 7);

  auto send_bad = [&](std::uint32_t seq, std::uint32_t fid,
                      std::uint16_t idx, std::uint16_t count) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = kFlagFrameEnd;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = fid;
    h.rtp_timestamp = 3000;
    h.frame_index = idx;
    h.frame_count = count;
    RtpHeader rtp;
    rtp.timestamp = 3000;
    rtp.marker = true;
    auto dg = *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                   std::vector<std::byte>(16, std::byte{1}));
    return rx.on_datagram(dg, clock.now());
  };

  const auto v0 = rx.counters().protocol_violations;
  auto out1 = send_bad(2000, 1, 64, 65);
  RUDP_CHECK(out1.accepted.empty());
  RUDP_CHECK_EQ(rx.storage_size(), 0u);
  auto out2 = send_bad(2001, 2, 65534, 65535);
  RUDP_CHECK(out2.accepted.empty());
  RUDP_CHECK_EQ(rx.storage_size(), 0u);
  RUDP_CHECK_EQ(rx.counters().protocol_violations, v0 + 2u);
}

RUDP_TEST(receiver_abandon_rejects_unbounded_end_and_antipodal) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 1000;
  rc.window_w = 64;
  UdpReceiver rx(rc, clock);
  rx.set_sync(1000, 7);

  const auto check_rejected = [&](std::vector<AbandonRange> ranges) {
    const auto v_before = rx.counters().protocol_violations;
    const auto r = rx.apply_abandon(std::move(ranges), clock.now());
    RUDP_CHECK(!r.applied);
    RUDP_CHECK(r.out_of_window_rejected);
    RUDP_CHECK_EQ(rx.recv_next(), 1000u);
    RUDP_CHECK_GT(rx.counters().protocol_violations, v_before);
  };

  // Start inside window, end past recv_next + window_w: must be rejected first.
  check_rejected({{1000u, 1200u}});
  // Huge 2^31-1 span: must be rejected immediately without hanging.
  check_rejected({{1000u, 1000u + 0x7FFFFFFEu}});
  // Straddling behind recv_next with end past window_w.
  check_rejected({{995u, 1100u}});
  // Start inside window, end wraps past antipode.
  check_rejected({{1000u + 63u, 1000u + 63u + 0x7FFFFFFFu}});
  // Antipodal start (exactly 2^31 ahead).
  check_rejected({{1000u + 0x80000000u, 1000u + 0x80000001u}});
  // Inverted range (start > end).
  check_rejected({{1005u, 1001u}});

  // Valid straddling range behind recv_next and inside window is clamped/applied.
  auto ok = rx.apply_abandon({{995u, 1005u}}, clock.now());
  RUDP_CHECK(ok.applied);
  RUDP_CHECK(!ok.out_of_window_rejected);
  RUDP_CHECK_EQ(rx.recv_next(), 1006u);

  // Valid wraparound at 2^32 boundary.
  UdpReceiver wrap_rx(rc, clock);
  wrap_rx.set_sync(0xFFFFFFFCu, 7);
  auto wok = wrap_rx.apply_abandon({{0xFFFFFFFAu, 2u}}, clock.now());
  RUDP_CHECK(wok.applied);
  RUDP_CHECK(!wok.out_of_window_rejected);
  RUDP_CHECK_EQ(wrap_rx.recv_next(), 3u);
  const auto wv = wrap_rx.counters().protocol_violations;
  auto wrej = wrap_rx.apply_abandon({{3u, 200u}}, clock.now());
  RUDP_CHECK(!wrej.applied);
  RUDP_CHECK(wrej.out_of_window_rejected);
  RUDP_CHECK_EQ(wrap_rx.recv_next(), 3u);
  RUDP_CHECK_GT(wrap_rx.counters().protocol_violations, wv);
}

RUDP_TEST(assembler_non_transitive_frame_ids_preserve_descriptor_check) {
  auto make_pkt = [](std::uint32_t fid, std::uint16_t idx, std::uint16_t count,
                     std::uint32_t rtp_ts) {
    AcceptedPacket p;
    p.frame_id = fid;
    p.seq = fid * 2u + idx;
    p.rtp_timestamp = rtp_ts;
    p.stream_id = 1;
    p.frame_index = idx;
    p.frame_count = count;
    p.frame_end = (idx + 1 == count);
    p.media.assign(8, static_cast<std::byte>(fid & 0xFFu));
    return p;
  };

  // Non-transitive modular frame_ids can cause RB-tree rotations that make an
  // inserted frame unreachable by find_frame(), bypassing INV-05 conflict checks.
  FrameAssembler asm_cycle(8);
  const std::uint32_t ids[8] = {
      0x60000000u, 0x00000000u, 0xA0000000u, 0x80000000u,
      0xC0000000u, 0xB8000000u, 0xD0000000u, 0xD8000000u};
  for (std::uint32_t fid : ids) {
    AcceptedPacket p0 = make_pkt(fid, 0, 2, 9000);
    RUDP_CHECK_EQ(static_cast<int>(asm_cycle.validate_descriptor(p0)),
                  static_cast<int>(DataClassification::VALID));
    RUDP_CHECK(asm_cycle.on_packet(std::move(p0)).expired_frames.empty());
  }
  RUDP_CHECK_EQ(asm_cycle.tracked_frames(), 8u);

  for (std::uint32_t fid : ids) {
    AcceptedPacket conflict_count = make_pkt(fid, 1, 3, 9000);
    RUDP_CHECK_EQ(static_cast<int>(asm_cycle.validate_descriptor(conflict_count)),
                  static_cast<int>(DataClassification::DESCRIPTOR_CONFLICT));
    AcceptedPacket conflict_ts = make_pkt(fid, 1, 2, 9001);
    RUDP_CHECK_EQ(static_cast<int>(asm_cycle.validate_descriptor(conflict_ts)),
                  static_cast<int>(DataClassification::DESCRIPTOR_CONFLICT));
  }

  // Inserting a 9th frame evicts one frame cleanly without UB.
  auto overflow_out = asm_cycle.on_packet(make_pkt(1u, 0, 2, 9000));
  RUDP_CHECK_EQ(overflow_out.expired_frames.size(), 1u);
  RUDP_CHECK_EQ(asm_cycle.tracked_frames(), 8u);

  // Serial wraparound overflow evicts the true oldest (0xFFFFFFFEu).
  FrameAssembler asm_wrap(4);
  const std::uint32_t wrap_ids[4] = {0xFFFFFFFFu, 0xFFFFFFFEu, 0u, 1u};
  for (std::uint32_t fid : wrap_ids) {
    RUDP_CHECK(asm_wrap.on_packet(make_pkt(fid, 0, 2, 3000)).expired_frames.empty());
  }
  auto wrap_out = asm_wrap.on_packet(make_pkt(2u, 0, 2, 3000));
  RUDP_CHECK_EQ(wrap_out.expired_frames.size(), 1u);
  RUDP_CHECK_EQ(wrap_out.expired_frames[0], 0xFFFFFFFEu);
  for (std::uint32_t fid : {0xFFFFFFFFu, 0u, 1u, 2u}) {
    auto done = asm_wrap.on_packet(make_pkt(fid, 1, 2, 3000));
    RUDP_CHECK_EQ(done.completed.size(), 1u);
    RUDP_CHECK_EQ(done.completed[0].frame_id, fid);
  }
}

RUDP_TEST(jitter_buffer_inserts_earlier_frame_ahead_of_later_frame) {
  auto make_cf = [](std::uint32_t fid, std::uint32_t rtp_ts) {
    CompletedFrame cf;
    cf.stream_id = 1;
    cf.frame_id = fid;
    cf.rtp_timestamp = rtp_ts;
    cf.frame_count = 1;
    cf.bytes.assign(16, static_cast<std::byte>(fid & 0xFFu));
    return cf;
  };

  JitterBuffer jb(4);
  jb.set_anchor(0, 0, ns_from_ms(100));
  RUDP_CHECK(jb.push(make_cf(1, 3000), ns_from_ms(10)).empty());
  RUDP_CHECK(jb.push(make_cf(0, 0), ns_from_ms(20)).empty());
  auto delivered = jb.on_timer(ns_from_ms(150));
  RUDP_CHECK_EQ(delivered.size(), 2u);
  RUDP_CHECK_EQ(delivered[0].frame.frame_id, 0u);
  RUDP_CHECK_EQ(delivered[1].frame.frame_id, 1u);

  // Overflow eviction must drop serial-oldest frame 1 when {2, 1, 3} arrive.
  JitterBuffer jb2(2);
  jb2.set_anchor(0, 0, ns_from_ms(200));
  RUDP_CHECK(jb2.push(make_cf(2, 6000), ns_from_ms(10)).empty());
  RUDP_CHECK(jb2.push(make_cf(1, 3000), ns_from_ms(15)).empty());
  RUDP_CHECK(jb2.push(make_cf(3, 9000), ns_from_ms(20)).empty());
  RUDP_CHECK_EQ(jb2.dropped(), 1u);
  auto d2 = jb2.on_timer(ns_from_ms(350));
  RUDP_CHECK_EQ(d2.size(), 2u);
  RUDP_CHECK_EQ(d2[0].frame.frame_id, 2u);
  RUDP_CHECK_EQ(d2[1].frame.frame_id, 3u);

  // Wraparound: pushing 0 then 0xFFFFFFFFu orders 0xFFFFFFFFu before 0.
  JitterBuffer jb_wrap(4);
  jb_wrap.set_anchor(0, 0, ns_from_ms(200));
  RUDP_CHECK(jb_wrap.push(make_cf(0u, 3000), ns_from_ms(10)).empty());
  RUDP_CHECK(jb_wrap.push(make_cf(0xFFFFFFFFu, 0), ns_from_ms(20)).empty());
  auto dw = jb_wrap.on_timer(ns_from_ms(250));
  RUDP_CHECK_EQ(dw.size(), 2u);
  RUDP_CHECK_EQ(dw[0].frame.frame_id, 0xFFFFFFFFu);
  RUDP_CHECK_EQ(dw[1].frame.frame_id, 0u);
}

RUDP_TEST(playout_next_deadline_includes_earlier_nack_deadline) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 1;
  rc.isn = 5000;
  rc.window_w = 64;
  rc.reorder_capacity = 256;
  rc.nack_throttle_srtt = ns_from_ms(25);
  PlayoutPipeline pipeline(rc, 128, 64, ns_from_ms(200), 64, clock);
  pipeline.set_sync(5000, 1);

  Header h;
  h.version = 1;
  h.header_len = 7;
  h.type = static_cast<std::uint8_t>(PacketType::DATA);
  h.flags = kFlagFrameEnd;
  h.stream_id = 1;
  h.seq = 5001;
  h.frame_id = 0;
  h.rtp_timestamp = 0;
  h.frame_index = 1;
  h.frame_count = 2;
  RtpHeader rtp;
  rtp.timestamp = 0;
  rtp.marker = true;
  auto dg = *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                 std::vector<std::byte>(32, std::byte{1}));

  pipeline.on_data(dg, ns_from_ms(10));
  const auto dl = pipeline.next_deadline();
  RUDP_CHECK(dl.has_value());
  RUDP_CHECK_EQ(*dl, ns_from_ms(35));
}

RUDP_TEST(playout_teardown_and_resync_clears_stale_frame_state) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 1;
  rc.isn = 100;
  rc.window_w = 64;
  rc.reorder_capacity = 256;
  rc.nack_throttle_srtt = ns_from_ms(25);
  PlayoutPipeline pipeline(rc, 128, 64, ns_from_ms(100), 64, clock);
  pipeline.set_sync(100, 1);

  auto make_dg = [](std::uint32_t seq, std::uint32_t fid, std::uint16_t idx,
                    std::uint16_t cnt, std::uint32_t rtp_ts) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = (idx + 1 == cnt) ? kFlagFrameEnd : 0;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = fid;
    h.rtp_timestamp = rtp_ts;
    h.frame_index = idx;
    h.frame_count = cnt;
    RtpHeader rtp;
    rtp.timestamp = rtp_ts;
    rtp.marker = (idx + 1 == cnt);
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(24, std::byte{2}));
  };

  // Session 1: deliver frame 0 (enters terminal_ids_), half-assemble frame 1,
  // and queue completed frame 2 in JitterBuffer.
  pipeline.on_data(make_dg(100, 0, 0, 1, 0), ns_from_ms(10));
  pipeline.on_timer(ns_from_ms(120));
  auto d1 = pipeline.take_deliveries();
  RUDP_CHECK_EQ(d1.size(), 1u);

  pipeline.on_data(make_dg(101, 1, 0, 2, 3000), ns_from_ms(130));
  pipeline.on_data(make_dg(103, 2, 0, 1, 9000), ns_from_ms(130));
  RUDP_CHECK_EQ(pipeline.jitter_depth(), 1u);

  pipeline.teardown();
  pipeline.set_sync(500, 2);
  RUDP_CHECK_EQ(pipeline.jitter_depth(), 0u);

  // Session 2 reuses frame_ids 0 and 1 with fresh descriptors and timeline.
  pipeline.on_data(make_dg(500, 0, 0, 1, 100000), ns_from_ms(200));
  pipeline.on_data(make_dg(501, 1, 0, 1, 103000), ns_from_ms(200));
  pipeline.on_timer(ns_from_ms(300));
  pipeline.on_timer(ns_from_ms(334));
  auto d2 = pipeline.take_deliveries();
  RUDP_CHECK_EQ(pipeline.receiver_counters().protocol_violations, 0u);
  RUDP_CHECK_EQ(d2.size(), 2u);
  RUDP_CHECK(d2[0].ontime);
  RUDP_CHECK(d2[1].ontime);
  RUDP_CHECK_EQ(d2[0].frame.frame_id, 0u);
  RUDP_CHECK_EQ(d2[0].frame.rtp_timestamp, 100000u);
  RUDP_CHECK_EQ(d2[1].frame.frame_id, 1u);
  RUDP_CHECK_EQ(d2[1].frame.rtp_timestamp, 103000u);
}

RUDP_TEST(receiver_single_ack_in_order_and_in_order_accepted_when_storage_full) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 5000;
  rc.window_w = 64;
  rc.reorder_capacity = 2;
  rc.nack_throttle_srtt = ns_from_ms(25);
  UdpReceiver rx(rc, clock);
  rx.set_sync(5000, 7);

  auto make_dg = [](std::uint32_t seq, std::uint32_t fid) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = kFlagFrameEnd;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = fid;
    h.rtp_timestamp = fid * 3000;
    h.frame_index = 0;
    h.frame_count = 1;
    RtpHeader rtp;
    rtp.timestamp = h.rtp_timestamp;
    rtp.marker = true;
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(32, std::byte{3}));
  };

  // In-order packet must produce exactly 1 cumulative ACK, never 2.
  auto out0 = rx.on_datagram(make_dg(5000, 0), clock.now());
  RUDP_CHECK_EQ(out0.accepted.size(), 1u);
  RUDP_CHECK_EQ(out0.datagrams.size(), 1u);
  RUDP_CHECK_EQ(rx.counters().acks_sent, 1u);
  RUDP_CHECK_EQ(rx.recv_next(), 5001u);

  // Fill reorder storage (capacity 2) with out-of-order 5002 and 5003.
  RUDP_CHECK_EQ(rx.on_datagram(make_dg(5002, 2), clock.now()).accepted.size(), 1u);
  RUDP_CHECK_EQ(rx.on_datagram(make_dg(5003, 3), clock.now()).accepted.size(), 1u);
  RUDP_CHECK_EQ(rx.storage_size(), 2u);

  // Another out-of-order packet 5004 hits reorder_capacity drop.
  auto out4 = rx.on_datagram(make_dg(5004, 4), clock.now());
  RUDP_CHECK(out4.accepted.empty());
  RUDP_CHECK_EQ(rx.counters().storage_full_drops, 1u);

  // In-order gap filler (seq == recv_next_ == 5001) must still be accepted
  // when storage_ is full so the head-of-line deadlock drains cleanly.
  auto out1 = rx.on_datagram(make_dg(5001, 1), clock.now());
  RUDP_CHECK_EQ(out1.accepted.size(), 1u);
  RUDP_CHECK_EQ(rx.recv_next(), 5004u);
  RUDP_CHECK_EQ(rx.storage_size(), 0u);
  RUDP_CHECK_EQ(out1.datagrams.size(), 1u);
}

RUDP_TEST(receiver_abandoned_range_drops_delayed_data_and_preserves_sack_bits) {
  VirtualClock clock;
  ReceiverConfig rc;
  rc.peer = Endpoint{1, 1};
  rc.stream_id = 1;
  rc.session_epoch = 7;
  rc.isn = 5000;
  rc.window_w = 64;
  rc.reorder_capacity = 256;
  rc.nack_throttle_srtt = ns_from_ms(25);
  UdpReceiver rx(rc, clock);
  rx.set_sync(5000, 7);

  auto make_dg = [](std::uint32_t seq, std::uint32_t fid) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = kFlagFrameEnd;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = fid;
    h.rtp_timestamp = fid * 3000;
    h.frame_index = 0;
    h.frame_count = 1;
    RtpHeader rtp;
    rtp.timestamp = h.rtp_timestamp;
    rtp.marker = true;
    return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                                std::vector<std::byte>(32, std::byte{4}));
  };

  auto extract_first_ack = [](const std::vector<Datagram>& dgs) {
    for (const auto& dg : dgs) {
      auto hdr = decode_header(dg.bytes);
      RUDP_CHECK(hdr.reject == RejectReason::NONE);
      if (static_cast<PacketType>(hdr.header.type) == PacketType::ACK) {
        auto ack = decode_ack(
            std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
        RUDP_CHECK(ack.reject == RejectReason::NONE);
        return ack.ack;
      }
    }
    RUDP_CHECK(false);
    return Ack{};
  };

  // Out-of-order seq 5005 creates hole at 5000..5004 and sets SACK bit 5.
  auto out5 = rx.on_datagram(make_dg(5005, 5), clock.now());
  Ack a1 = extract_first_ack(out5.datagrams);
  RUDP_CHECK_EQ(a1.ack_number, 4999u);
  RUDP_CHECK((a1.sack_bitmap & (1ull << 5)) != 0u);

  // Abandon [5005, 5007] while 5000 is still missing (recv_next_ remains 5000):
  // fresh ACK must NOT renege SACK bit 5 and must also cover [5005..5007].
  auto ar = rx.apply_abandon({{5005u, 5007u}}, clock.now());
  RUDP_CHECK(ar.applied);
  RUDP_CHECK_EQ(rx.recv_next(), 5000u);
  Ack a2 = extract_first_ack(ar.acks);
  RUDP_CHECK_EQ(a2.ack_number, 4999u);
  RUDP_CHECK((a2.sack_bitmap & (1ull << 5)) != 0u);
  RUDP_CHECK((a2.sack_bitmap & (1ull << 6)) != 0u);
  RUDP_CHECK((a2.sack_bitmap & (1ull << 7)) != 0u);

  // Delayed DATA inside abandoned [5005, 5007] with unseen frame_id must drop
  // before entering storage_ (INV-08).
  const auto dead_before = rx.counters().dead_frame_drops;
  auto out_delayed = rx.on_datagram(make_dg(5006, 99), clock.now());
  RUDP_CHECK(out_delayed.accepted.empty());
  RUDP_CHECK_EQ(rx.storage_size(), 0u);
  RUDP_CHECK_EQ(rx.counters().dead_frame_drops, dead_before + 1u);
}

RUDP_TEST(sender_never_retransmits_cumulatively_acked_packets) {
  SenderHarness h;
  h.offer(0, 3000, ns_from_ms(500));
  auto dgs = h.pump_new();
  RUDP_CHECK_EQ(dgs.size(), 3u);  // seqs 1000, 1001, 1002

  // Cumulatively ACK seq 1000; 1001 and 1002 remain in flight in the same frame.
  h.clock.advance(ns_from_ms(10));
  Ack ack1;
  ack1.session_epoch = 55;
  ack1.ack_number = 1000;
  h.sender->on_ack(ack1, h.clock.now());

  // Advance past RTO (~125ms) but before deadline (500ms): 1000 must NOT be
  // retransmitted; only 1001 and 1002 are eligible.
  h.clock.advance(ns_from_ms(300));  // now = 310ms
  h.sender->on_timer(h.clock.now());
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.data.size(), 2u);
  for (const auto& dg : out.data) {
    auto dec = decode_header(dg.bytes);
    RUDP_CHECK(dec.reject == RejectReason::NONE);
    RUDP_CHECK(dec.header.seq != 1000u);
  }

  // SACK 1001 and 1002 while 1000 is cumulatively ACKed: ADR-0011 all_held
  // 1-RTO grace must hold at exact deadline (500ms) instead of abandoning.
  Ack ack2;
  ack2.session_epoch = 55;
  ack2.ack_number = 1000;
  ack2.sack_bitmap = (1ull << 0) | (1ull << 1);  // seqs 1001, 1002
  h.sender->on_ack(ack2, h.clock.now());
  h.clock.set(ns_from_ms(500));
  h.sender->on_timer(h.clock.now());
  auto grace_out = h.sender->take_output();
  RUDP_CHECK(grace_out.closures.empty());
  const auto dl = h.sender->next_deadline();
  RUDP_CHECK(dl.has_value());
  RUDP_CHECK_GT(*dl, ns_from_ms(500));
}

RUDP_TEST(sender_no_rtt_sample_from_abandoned_frame_ack) {
  SenderHarness h;
  h.offer(0, 1000, ns_from_ms(40));
  auto dgs = h.pump_new();
  RUDP_CHECK_EQ(dgs.size(), 1u);

  // Abandon frame 0 on deadline expiration.
  h.clock.advance(ns_from_ms(50));
  h.sender->on_timer(h.clock.now());
  auto out = h.sender->take_output();
  RUDP_CHECK_EQ(out.closures.size(), 1u);
  RUDP_CHECK(out.closures[0].outcome == SessionFrameOutcome::ABANDONED);

  // Cumulative ACK advancing across the abandoned stub must not sample RTT.
  h.clock.advance(ns_from_ms(15));
  Ack ack;
  ack.session_epoch = 55;
  ack.ack_number = 1000;
  h.sender->on_ack(ack, h.clock.now());
  RUDP_CHECK_EQ(h.sender->counters().rtt_samples, 0u);
}

RUDP_TEST(client_node_bytes_owned_tracks_assembly_jitter_and_deliveries) {
  VirtualClock clock;
  Config cfg;
  cfg.playout_delay = ns_from_ms(100);
  const Endpoint gw_ep{1u, 40000u};
  ClientNode client(cfg, clock, gw_ep, 1, [] { return 19u; });

  struct RecvNet final : INetwork {
    std::vector<Datagram> sent;
    SendResult send(const Endpoint& peer,
                    std::span<const std::byte> bytes) override {
      Datagram dg;
      dg.peer = peer;
      dg.bytes.assign(bytes.begin(), bytes.end());
      sent.push_back(std::move(dg));
      return {SendStatus::SENT};
    }
    RecvResult receive(std::span<std::byte>) override {
      return {RecvStatus::WOULD_BLOCK, {}, 0};
    }
  } net;

  clock.set(0);
  client.start_join(0);
  (void)client.pump(net, 0);
  auto jping = decode_ping(
      std::span<const std::byte>(net.sent[0].bytes).subspan(kHeaderSize));

  Pong pong;
  pong.nonce = jping.ping.nonce;
  pong.status = PongStatus::OK;
  pong.session_epoch = 55;
  pong.server_isn = 1000;
  Datagram pong_dg;
  pong_dg.peer = gw_ep;
  pong_dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
  Header ph;
  ph.version = kWireVersion;
  ph.header_len = kHeaderLenWords;
  ph.type = static_cast<std::uint8_t>(PacketType::PONG);
  ph.stream_id = 1;
  ph.payload_len = static_cast<std::uint16_t>(encoded_pong_size());
  encode_header(pong_dg.bytes.data(), ph);
  encode_pong(pong_dg.bytes.data() + kHeaderSize, pong);
  client.on_datagram(pong_dg, 0);
  RUDP_CHECK(client.established());
  RUDP_CHECK_EQ(client.bytes_owned(), 0u);

  auto make_frag = [&](std::uint32_t seq, std::uint16_t idx, std::uint16_t cnt) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.flags = (idx + 1 == cnt) ? kFlagFrameEnd : 0;
    h.stream_id = 1;
    h.seq = seq;
    h.frame_id = 0;
    h.rtp_timestamp = 0;
    h.frame_index = idx;
    h.frame_count = cnt;
    RtpHeader rtp;
    rtp.timestamp = 0;
    rtp.marker = (idx + 1 == cnt);
    return *build_data_datagram(gw_ep, h, rtp,
                                std::vector<std::byte>(100, std::byte{0x5}));
  };

  clock.set(ns_from_ms(10));
  client.on_datagram(make_frag(1000, 0, 2), clock.now());
  RUDP_CHECK_EQ(client.bytes_owned(), 100u);  // in FrameAssembler

  client.on_datagram(make_frag(1001, 1, 2), clock.now());
  RUDP_CHECK_EQ(client.bytes_owned(), 200u);  // completed into JitterBuffer

  clock.set(ns_from_ms(110));
  client.on_timer(clock.now());
  RUDP_CHECK_EQ(client.bytes_owned(), 200u);  // sitting in pending_deliveries_

  auto deliveries = client.take_deliveries();
  RUDP_CHECK_EQ(deliveries.size(), 1u);
  RUDP_CHECK_EQ(client.bytes_owned(), 0u);    // drained by caller
}

RUDP_TEST(receiver_acks_retransmissions_and_preserves_completed_jitter_frames) {
  // 1) --no-deadline full window with dropped initial ACKs + duplicate SACK:
  //    retransmissions within window_w behind recv_next must elicit idempotent
  //    ACKs so the sender is never wedged; duplicates in storage_ re-emit SACKs.
  {
    VirtualClock clock;
    SenderConfig sc;
    sc.stream_id = 1;
    sc.ssrc = 7;
    sc.session_epoch = 5;
    sc.isn = 1000;
    sc.window_w = 10;
    sc.max_frame_packets = 10;
    sc.media_payload_bytes = 1200;
    sc.deadline_mode = false;
    TransportSender tx(sc, clock);

    ReceiverConfig rc;
    rc.peer = Endpoint{1, 1};
    rc.stream_id = 1;
    rc.session_epoch = 5;
    rc.isn = 1000;
    rc.window_w = 10;
    rc.reorder_capacity = 32;
    UdpReceiver rx(rc, clock);
    rx.set_sync(1000, 5);

    auto make_frame = [&](std::uint32_t fid) {
      MediaFrame f;
      f.stream_id = 1;
      f.frame_id = fid;
      f.rtp_timestamp = fid * 3000u;
      f.capture_time = clock.now();
      f.deadline_budget = ns_from_ms(100);
      f.bytes.assign(12000, std::byte{0x3});
      return f;
    };

    tx.on_frame(make_frame(0));
    auto adm0 = tx.take_output();
    RUDP_CHECK_EQ(adm0.admissions.size(), 1u);
    RUDP_CHECK(adm0.admissions[0].admitted);

    for (int i = 0; i < 10; ++i) {
      auto dg = tx.emit_new_data(clock.now(), 1 << 20);
      RUDP_CHECK(dg.has_value());
      (void)rx.on_datagram(*dg, clock.now());  // drop all uplink ACKs
    }
    RUDP_CHECK_EQ(rx.recv_next(), 1010u);
    RUDP_CHECK_EQ(tx.allocated_unacked(), 10u);

    // Frame 1 blocked because window is full (initial ACKs were lost).
    tx.on_frame(make_frame(1));
    auto adm1 = tx.take_output();
    RUDP_CHECK(!adm1.admissions[0].admitted);

    // Ancient seq far behind recv_next - W must NOT emit an ACK.
    Header old_h;
    old_h.version = kWireVersion;
    old_h.header_len = kHeaderLenWords;
    old_h.type = static_cast<std::uint8_t>(PacketType::DATA);
    old_h.flags = kFlagFrameEnd;
    old_h.stream_id = 1;
    old_h.seq = 900;
    old_h.frame_id = 99;
    old_h.frame_count = 1;
    RtpHeader old_rtp;
    auto old_dg = *build_data_datagram(
        Endpoint{1, 1}, old_h, old_rtp, std::vector<std::byte>(16, std::byte{0}));
    auto old_out = rx.on_datagram(old_dg, clock.now());
    RUDP_CHECK(old_out.datagrams.empty());

    // RTO fires at 75ms: retransmitting consumed seq 1000 elicits fresh ACK.
    clock.advance(ns_from_ms(75));
    tx.on_timer(clock.now());
    auto rtx = tx.take_output();
    RUDP_CHECK(!rtx.data.empty());
    auto ack_out = rx.on_datagram(rtx.data[0], clock.now());
    RUDP_CHECK_EQ(rx.counters().too_old, 2u);
    RUDP_CHECK_EQ(ack_out.datagrams.size(), 1u);
    auto dec_ack = decode_ack(
        std::span<const std::byte>(ack_out.datagrams[0].bytes).subspan(kHeaderSize));
    RUDP_CHECK(dec_ack.reject == RejectReason::NONE);
    RUDP_CHECK_EQ(dec_ack.ack.ack_number, 1009u);

    tx.on_ack(dec_ack.ack, clock.now());
    RUDP_CHECK(tx.window_empty());

    // Out-of-order duplicate in storage_ re-emits SACK-bearing ACK.
    Header h2;
    h2.version = kWireVersion;
    h2.header_len = kHeaderLenWords;
    h2.type = static_cast<std::uint8_t>(PacketType::DATA);
    h2.flags = kFlagFrameEnd;
    h2.stream_id = 1;
    h2.seq = 1012;
    h2.frame_id = 2;
    h2.rtp_timestamp = 6000;
    h2.frame_count = 1;
    RtpHeader rtp2;
    rtp2.timestamp = 6000;
    rtp2.marker = true;
    auto dg2 = *build_data_datagram(
        Endpoint{1, 1}, h2, rtp2, std::vector<std::byte>(16, std::byte{7}));
    (void)rx.on_datagram(dg2, clock.now());
    auto dup_out = rx.on_datagram(dg2, clock.now());
    RUDP_CHECK_EQ(rx.counters().duplicates, 1u);
    RUDP_CHECK_EQ(dup_out.datagrams.size(), 1u);
    auto sack_ack = decode_ack(
        std::span<const std::byte>(dup_out.datagrams[0].bytes).subspan(kHeaderSize));
    RUDP_CHECK_EQ(sack_ack.ack.ack_number, 1009u);
    RUDP_CHECK((sack_ack.ack.sack_bitmap & (1ull << 2)) != 0u);
  }

  // 2) Descriptor conflict kills assembly/delivery of frame 0 without wedging
  //    recv_next_ on subsequent valid packets of frame 0.
  {
    VirtualClock clock;
    ReceiverConfig rc;
    rc.peer = Endpoint{1, 1};
    rc.stream_id = 1;
    rc.session_epoch = 1;
    rc.isn = 100;
    rc.window_w = 64;
    rc.reorder_capacity = 256;
    PlayoutPipeline pipeline(rc, 64, 64, ns_from_ms(60), 64, clock);
    pipeline.set_sync(100, 1);

    auto make_dg = [](std::uint32_t seq, std::uint32_t fid, std::uint16_t idx,
                      std::uint16_t cnt, std::uint32_t rtp_ts) {
      Header h;
      h.version = kWireVersion;
      h.header_len = kHeaderLenWords;
      h.type = static_cast<std::uint8_t>(PacketType::DATA);
      h.flags = (idx + 1 == cnt) ? kFlagFrameEnd : 0;
      h.stream_id = 1;
      h.seq = seq;
      h.frame_id = fid;
      h.rtp_timestamp = rtp_ts;
      h.frame_index = idx;
      h.frame_count = cnt;
      RtpHeader rtp;
      rtp.timestamp = rtp_ts;
      rtp.marker = (idx + 1 == cnt);
      return *build_data_datagram(
          Endpoint{1, 1}, h, rtp, std::vector<std::byte>(24, std::byte{1}));
    };

    (void)pipeline.on_data(make_dg(100, 0, 0, 2, 3000), ns_from_ms(10));
    // Conflicting descriptor on seq 101: rejected, frame 0 marked dead.
    (void)pipeline.on_data(make_dg(101, 0, 1, 2, 9999), ns_from_ms(12));
    RUDP_CHECK_EQ(pipeline.receiver_counters().protocol_violations, 1u);
    RUDP_CHECK_EQ(pipeline.recv_next(), 101u);
    // Legitimate packet for seq 101 advances recv_next without completing frame 0.
    (void)pipeline.on_data(make_dg(101, 0, 1, 2, 3000), ns_from_ms(15));
    RUDP_CHECK_EQ(pipeline.recv_next(), 102u);
    pipeline.on_timer(ns_from_ms(120));
    RUDP_CHECK_EQ(pipeline.delivered_ontime() + pipeline.delivered_late(), 0u);
  }

  // 3) Out-of-order completed frame sitting in JitterBuffer survives late
  //    ABANDON covering its sequence range and delivers ONTIME.
  {
    VirtualClock clock;
    ReceiverConfig rc;
    rc.peer = Endpoint{1, 1};
    rc.stream_id = 1;
    rc.session_epoch = 1;
    rc.isn = 200;
    rc.window_w = 64;
    rc.reorder_capacity = 256;
    PlayoutPipeline pipeline(rc, 64, 64, ns_from_ms(100), 64, clock);
    pipeline.set_sync(200, 1);

    auto make_dg = [](std::uint32_t seq, std::uint32_t fid, std::uint16_t idx,
                      std::uint16_t cnt, std::uint32_t rtp_ts) {
      Header h;
      h.version = kWireVersion;
      h.header_len = kHeaderLenWords;
      h.type = static_cast<std::uint8_t>(PacketType::DATA);
      h.flags = (idx + 1 == cnt) ? kFlagFrameEnd : 0;
      h.stream_id = 1;
      h.seq = seq;
      h.frame_id = fid;
      h.rtp_timestamp = rtp_ts;
      h.frame_index = idx;
      h.frame_count = cnt;
      RtpHeader rtp;
      rtp.timestamp = rtp_ts;
      rtp.marker = (idx + 1 == cnt);
      return *build_data_datagram(
          Endpoint{1, 1}, h, rtp, std::vector<std::byte>(24, std::byte{2}));
    };

    // seq 200 (frame 0) missing; frame 1 (201..202) arrives complete into jitter.
    (void)pipeline.on_data(make_dg(201, 1, 0, 2, 3000), ns_from_ms(10));
    (void)pipeline.on_data(make_dg(202, 1, 1, 2, 3000), ns_from_ms(12));
    RUDP_CHECK_EQ(pipeline.recv_next(), 200u);
    RUDP_CHECK_EQ(pipeline.jitter_depth(), 1u);

    // Delayed ABANDON covering [201, 202] must not destroy completed frame 1.
    (void)pipeline.apply_abandon({{201u, 202u}}, ns_from_ms(50));
    RUDP_CHECK_EQ(pipeline.jitter_depth(), 1u);
    RUDP_CHECK_EQ(pipeline.playout_dropped(), 0u);
    RUDP_CHECK_EQ(pipeline.delivery_abandoned(), 0u);

    pipeline.on_timer(ns_from_ms(110));
    auto delivered = pipeline.take_deliveries();
    RUDP_CHECK_EQ(delivered.size(), 1u);
    RUDP_CHECK_EQ(delivered[0].frame.frame_id, 1u);
    RUDP_CHECK_EQ(pipeline.delivered_ontime(), 1u);
  }
}

RUDP_TEST(sender_preadmission_drop_and_mid_gop_join_request_keyframe_and_prune) {
  // 1) Mid-GOP join prunes initial DELTA_FRAMEs, requests a keyframe once,
  //    and preserves SYNC_START on the first admitted KEY_FRAME.
  {
    VirtualClock clock;
    SenderConfig sc;
    sc.stream_id = 1;
    sc.ssrc = 7;
    sc.session_epoch = 55;
    sc.isn = 1000;
    sc.window_w = 10;
    sc.max_frame_packets = 10;
    sc.media_payload_bytes = 1200;
    TransportSender sender(sc, clock);

    // Oversized frame report preserves actual frame_id (42), not 0.
    MediaFrame huge;
    huge.stream_id = 1;
    huge.frame_id = 42;
    huge.capture_time = clock.now();
    huge.deadline_budget = ns_from_ms(100);
    huge.bytes.assign(20000, std::byte{0x1});
    sender.on_frame(std::move(huge));
    auto huge_out = sender.take_output();
    RUDP_CHECK_EQ(huge_out.admissions.size(), 1u);
    RUDP_CHECK(!huge_out.admissions[0].admitted);
    RUDP_CHECK_EQ(huge_out.admissions[0].frame_id, 42u);

    // Delta frames arriving before any admitted KEY_FRAME are pruned.
    SenderConfig sc2 = sc;
    TransportSender sender2(sc2, clock);
    MediaFrame d10;
    d10.stream_id = 1;
    d10.frame_id = 10;
    d10.frame_type = FrameType::DELTA_FRAME;
    d10.depends_on_frame = 9;
    d10.capture_time = clock.now();
    d10.deadline_budget = ns_from_ms(100);
    d10.bytes.assign(1200, std::byte{0x2});
    sender2.on_frame(std::move(d10));
    auto out10 = sender2.take_output();
    RUDP_CHECK(!out10.admissions[0].admitted);
    RUDP_CHECK(out10.keyframe_requested);
    RUDP_CHECK_EQ(sender2.counters().cascade_abandons, 1u);
    RUDP_CHECK_EQ(sender2.allocated_unacked(), 0u);

    // First KEY_FRAME arrives: admitted cleanly and carries kFlagSyncStart.
    MediaFrame k11;
    k11.stream_id = 1;
    k11.frame_id = 11;
    k11.frame_type = FrameType::KEY_FRAME;
    k11.capture_time = clock.now();
    k11.deadline_budget = ns_from_ms(100);
    k11.bytes.assign(1200, std::byte{0x3});
    sender2.on_frame(std::move(k11));
    auto out11 = sender2.take_output();
    RUDP_CHECK(out11.admissions[0].admitted);
    auto dg11 = sender2.emit_new_data(clock.now(), 1 << 20);
    RUDP_CHECK(dg11.has_value());
    auto dec11 = decode_header(dg11->bytes);
    RUDP_CHECK((dec11.header.flags & kFlagSyncStart) != 0u);
  }

  // 2) Pre-admission window-full drop of KEY_FRAME triggers keyframe request
  //    and prunes subsequent dependent DELTA_FRAMEs once window opens.
  {
    VirtualClock clock;
    SenderConfig sc;
    sc.stream_id = 1;
    sc.ssrc = 7;
    sc.session_epoch = 55;
    sc.isn = 2000;
    sc.window_w = 10;
    sc.max_frame_packets = 10;
    sc.media_payload_bytes = 1200;
    TransportSender sender(sc, clock);

    MediaFrame k20;
    k20.stream_id = 1;
    k20.frame_id = 20;
    k20.frame_type = FrameType::KEY_FRAME;
    k20.capture_time = clock.now();
    k20.deadline_budget = ns_from_ms(100);
    k20.bytes.assign(12000, std::byte{0x4});
    sender.on_frame(std::move(k20));
    RUDP_CHECK(sender.take_output().admissions[0].admitted);

    // Window full: KEY_FRAME 21 dropped pre-admission -> requests keyframe.
    MediaFrame k21;
    k21.stream_id = 1;
    k21.frame_id = 21;
    k21.frame_type = FrameType::KEY_FRAME;
    k21.capture_time = clock.now();
    k21.deadline_budget = ns_from_ms(100);
    k21.bytes.assign(12000, std::byte{0x5});
    sender.on_frame(std::move(k21));
    auto out21 = sender.take_output();
    RUDP_CHECK(!out21.admissions[0].admitted);
    RUDP_CHECK(out21.keyframe_requested);

    // Drain frame 20 so window opens again.
    Ack ack;
    ack.session_epoch = 55;
    ack.ack_number = 2009;
    sender.on_ack(ack, clock.now());
    RUDP_CHECK(sender.window_empty());

    // Dependent DELTA_FRAME 22 referencing dropped KEY_FRAME 21 is pruned.
    MediaFrame d22;
    d22.stream_id = 1;
    d22.frame_id = 22;
    d22.frame_type = FrameType::DELTA_FRAME;
    d22.depends_on_frame = 21;
    d22.capture_time = clock.now();
    d22.deadline_budget = ns_from_ms(100);
    d22.bytes.assign(1200, std::byte{0x6});
    sender.on_frame(std::move(d22));
    auto out22 = sender.take_output();
    RUDP_CHECK(!out22.admissions[0].admitted);
    RUDP_CHECK_EQ(sender.counters().cascade_abandons, 1u);

    // Next fresh KEY_FRAME 23 is admitted cleanly.
    MediaFrame k23;
    k23.stream_id = 1;
    k23.frame_id = 23;
    k23.frame_type = FrameType::KEY_FRAME;
    k23.capture_time = clock.now();
    k23.deadline_budget = ns_from_ms(100);
    k23.bytes.assign(1200, std::byte{0x7});
    sender.on_frame(std::move(k23));
    RUDP_CHECK(sender.take_output().admissions[0].admitted);
  }
}

}  // namespace rudp_test
