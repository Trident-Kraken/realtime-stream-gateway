// core.cpp — serial arithmetic, RTO estimation, deadline rounding, config
// validation, and range normalization (Phase 1 gate).

#include "test_framework.hpp"

#include "rudp_rt/config.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/wire.hpp"

#include <algorithm>
#include <set>
#include <vector>

namespace rudp_test {

using namespace rudp_rt;

RUDP_TEST(serial_arithmetic_wrap) {
  // RFC 1982-style comparisons across the 2^32 boundary.
  RUDP_CHECK(seq_lt(0xFFFFFFFFu, 0u));
  RUDP_CHECK(seq_lt(0xFFFFFFFEu, 0xFFFFFFFFu));
  RUDP_CHECK(seq_lt(0x7FFFFFFFu, 0x80000000u));
  // Distance exactly 2^31 is excluded from the comparison domain
  // (spec §4.5: every compared distance must be < 2^31); not asserted.
  RUDP_CHECK(seq_le(5u, 5u));
  // In-domain larger checks (distances < 2^31 per spec §4.5).
  RUDP_CHECK(seq_gt(6u, 5u));
  RUDP_CHECK(seq_ge(6u, 5u));
  RUDP_CHECK(seq_gt(0xFFFFFFFFu, 0xFFFFFFFEu));
  RUDP_CHECK_EQ(seq_forward(0xFFFFFFFFu, 1u), 2ull);
  RUDP_CHECK_EQ(seq_forward(100u, 164u), 64ull);
  RUDP_CHECK_EQ(seq_advance(0xFFFFFFFEu, 3u), 1u);
  RUDP_CHECK_EQ(seq_next(0xFFFFFFFFu), 0u);
  RUDP_CHECK_EQ(seq_prev(0u), 0xFFFFFFFFu);

  // Frame ids use the same arithmetic.
  RUDP_CHECK(frame_lt(0xFFFFFFFFu, 0u));
}

RUDP_TEST(rto_estimator_rfc6298) {
  RtoEstimator est;
  est.configure(ns_from_ms(25), ns_from_ms(25) / 2, ns_from_ms(50),
                ns_from_ms(1000));
  // Before any sample: RTO = 25 + max(1, 4*12.5) = 75 ms.
  RUDP_CHECK_EQ(est.current(), ns_from_ms(75));

  // First sample: SRTT = R, RTTVAR = R/2 (RFC 6298 §2.2).
  est.sample(ns_from_ms(10));
  RUDP_CHECK_EQ(est.srtt(), ns_from_ms(10));
  RUDP_CHECK_EQ(est.rttvar(), ns_from_ms(5));
  // RTO = 10 + max(1, 20) = 30 -> clamped to RTO_MIN = 50.
  RUDP_CHECK_EQ(est.current(), ns_from_ms(50));

  est.sample(ns_from_ms(30));
  // RTTVAR = 0.75*5 + 0.25*|10-30| = 8.75; SRTT = 0.875*10 + 0.125*30 = 12.5.
  RUDP_CHECK_EQ(est.rttvar(), (3 * ns_from_ms(5) + ns_from_ms(20)) / 4);
  RUDP_CHECK_EQ(est.srtt(), (7 * ns_from_ms(10) + ns_from_ms(30)) / 8);
}

RUDP_TEST(sender_deadline_rounding) {
  // remaining_ms = ceil(remaining / 1ms), saturated at 65535, zero = expired.
  VirtualClock clock;
  SenderConfig cfg;
  cfg.stream_id = 1;
  cfg.ssrc = 7;
  cfg.session_epoch = 99;
  cfg.isn = 1000;
  cfg.window_w = 64;
  cfg.max_frame_packets = 10;
  cfg.media_payload_bytes = 1200;
  TransportSender sender(cfg, clock);

  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 0;
  frame.rtp_timestamp = 0;
  frame.capture_time = 0;
  frame.deadline_budget = ns_from_ms(100);
  frame.bytes.assign(12000, std::byte{0xC});
  sender.on_frame(std::move(frame));

  clock.advance(ns_from_ms(3));  // remaining 97ms -> 97
  auto dg = sender.emit_new_data(clock.now(), 1 << 20);
  RUDP_CHECK(dg.has_value());
  auto decoded = decode_header(dg->bytes);
  RUDP_CHECK(decoded.reject == RejectReason::NONE);
  RUDP_CHECK_EQ(decoded.header.deadline_ms, 97u);

  // Sub-millisecond remaining rounds up (ceil): 100ms - 99.2ms = 0.8ms -> 1.
  MediaFrame frame2;
  frame2.stream_id = 1;
  frame2.frame_id = 1;
  frame2.capture_time = 0;
  frame2.deadline_budget = ns_from_ms(100);
  frame2.bytes.assign(12000, std::byte{0xC});
  sender.on_frame(std::move(frame2));
  clock.advance(ns_from_ms(96) + 280 * kNsPerUs);  // now = 99.28ms
  auto dg2 = sender.emit_new_data(clock.now(), 1 << 20);
  // remaining = 100ms - 99.28ms = 0.72ms -> ceil = 1
  RUDP_CHECK(dg2.has_value());
  auto d2 = decode_header(dg2->bytes);
  RUDP_CHECK_EQ(d2.header.deadline_ms, 1u);
}

RUDP_TEST(config_validation_constraints) {
  Config base;
  RUDP_CHECK(validate_config(base).empty());

  {
    Config c = base;
    c.window_w = 65;
    auto errs = validate_config(c);
    RUDP_CHECK(!errs.empty());
  }
  {
    Config c = base;
    c.max_frame_packets = c.window_w + 1;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.media_payload_bytes = 1201;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.deadline_budget = 0;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.rto_min = ns_from_ms(100);
    c.rto_max = ns_from_ms(50);
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.max_sessions = 0;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.control_reserve_bytes =
        static_cast<std::int64_t>(c.control_queue_datagrams) * 1240;
    RUDP_CHECK(!validate_config(c).empty());  // must be strictly less
  }
  {
    Config c = base;
    c.token_burst_bytes = 1000;  // below one max datagram
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.capture_queue_frames = 0;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.keepalive_min_interval = ns_from_ms(100);  // < silence_after (200ms)
    RUDP_CHECK(!validate_config(c).empty());
  }
}

RUDP_TEST(config_toml_subset) {
  const std::string toml = R"(
# comment
[gateway]
port = 51000
stream_id = 3      # trailing comment

[media]
frame_bytes = 6000
deadline_budget = "250ms"

[experiment]
seed = 123
)";
  Config cfg = load_config_from_string(toml);
  RUDP_CHECK_EQ(cfg.port, 51000u);
  RUDP_CHECK_EQ(cfg.stream_id, 3u);
  RUDP_CHECK_EQ(cfg.frame_bytes, 6000);
  RUDP_CHECK_EQ(cfg.deadline_budget, ns_from_ms(250));
  RUDP_CHECK_EQ(cfg.seed, 123ull);

  bool threw = false;
  try {
    load_config_from_string("[bogus]\nkey = 1\n");
  } catch (const std::exception&) {
    threw = true;
  }
  RUDP_CHECK(threw);  // unknown keys are startup errors (ADR-0014)
}

RUDP_TEST(duration_parsing) {
  RUDP_CHECK_EQ(parse_duration("100ms"), ns_from_ms(100));
  RUDP_CHECK_EQ(parse_duration("2s"), 2 * kNsPerSec);
  RUDP_CHECK_EQ(parse_duration("500us"), 500 * kNsPerUs);
  RUDP_CHECK_EQ(parse_duration("1m"), 60 * kNsPerSec);
  RUDP_CHECK_EQ(parse_duration("42"), 42);
}

RUDP_TEST(abandon_range_normalization) {
  // Sort, merge overlap and adjacency (spec §4.5).
  auto merged = normalize_abandon_ranges({
      {50, 60}, {10, 20}, {21, 30}, {40, 45}, {44, 48},
  });
  RUDP_CHECK_EQ(merged.size(), 3u);
  RUDP_CHECK((merged[0] == AbandonRange{10, 30}));
  RUDP_CHECK((merged[1] == AbandonRange{40, 48}));
  RUDP_CHECK((merged[2] == AbandonRange{50, 60}));

  // Empty and invalid ranges are dropped.
  auto empty = normalize_abandon_ranges({{5, 3}, {7, 7}});
  RUDP_CHECK_EQ(empty.size(), 1u);
  RUDP_CHECK((empty[0] == AbandonRange{7, 7}));
}

RUDP_TEST(packetizer_fragmentation) {
  Packetizer pz;
  MediaFrame frame;
  frame.stream_id = 1;
  frame.frame_id = 5;
  frame.rtp_timestamp = 15000;
  frame.bytes.assign(12000, std::byte{0xA});
  auto pf = pz.packetize(std::move(frame), 0x5EED0001u, 1200, 10);
  RUDP_CHECK(pf.has_value());
  RUDP_CHECK_EQ(pf->frame_count(), 10u);
  for (std::size_t i = 0; i < pf->fragments.size(); ++i) {
    RUDP_CHECK_EQ(pf->fragments[i].len, 1200u);
    RUDP_CHECK_EQ(pf->fragments[i].index, static_cast<std::uint16_t>(i));
    RUDP_CHECK_EQ(pf->fragments[i].frame_end, i + 1 == pf->fragments.size());
  }
  // Too many packets for the configuration: rejected.
  MediaFrame big;
  big.frame_id = 6;
  big.bytes.assign(12001, std::byte{0xB});
  auto too_big = pz.packetize(std::move(big), 1, 1200, 10);
  RUDP_CHECK(!too_big.has_value());
}

}  // namespace rudp_test
