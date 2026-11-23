// core.cpp — serial arithmetic, RTO estimation, deadline rounding, config
// validation, and range normalization (Phase 1 gate).

#include "test_framework.hpp"

#include "rudp_rt/config.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/sim.hpp"
#include "rudp_rt/wire.hpp"

#include <algorithm>
#include <limits>
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

RUDP_TEST(session_ledger_independent_entrance) {
  SessionFrameLedger ledger;
  ++ledger.offered_count;
  RUDP_CHECK(!ledger.conserved());  // a lost admission is observable
  ledger.admit();
  RUDP_CHECK(ledger.conserved());
  ledger.close(SessionFrameOutcome::ONTIME);
  RUDP_CHECK(ledger.conserved());
  ++ledger.ontime;  // duplicate closure/corrupted terminal count
  RUDP_CHECK(!ledger.conserved());
}

RUDP_TEST(seed_decimal_and_hexadecimal) {
  RUDP_CHECK_EQ(parse_seed("0x5EED"), 24301u);
  RUDP_CHECK_EQ(parse_seed("24301"), 24301u);
  RUDP_CHECK_EQ(parse_seed("00010"), 10u);
  bool rejected = false;
  try { (void)parse_seed("24301suffix"); }
  catch (const std::exception&) { rejected = true; }
  RUDP_CHECK(rejected);
}

RUDP_TEST(abandon_range_normalization_cyclic_safe) {
  // Wraparound adjacency across 2^32 merges cleanly.
  auto wrapped = normalize_abandon_ranges({{2u, 5u}, {0xFFFFFFFEu, 1u}});
  RUDP_CHECK_EQ(wrapped.size(), 1u);
  RUDP_CHECK((wrapped[0] == AbandonRange{0xFFFFFFFEu, 5u}));

  // Adversarial 3-cycle and antipodal pair must not cause std::sort UB.
  auto tri = normalize_abandon_ranges(
      {{0u, 1u}, {0x60000000u, 0x60000001u}, {0xC0000000u, 0xC0000001u}});
  RUDP_CHECK_EQ(tri.size(), 3u);

  auto antipodal = normalize_abandon_ranges(
      {{0u, 1u}, {0x80000000u, 0x80000001u}});
  RUDP_CHECK_EQ(antipodal.size(), 2u);

  // Antipodal-span range (>= 2^31) is invalid and dropped.
  auto huge = normalize_abandon_ranges({{0u, 0x80000000u}});
  RUDP_CHECK(huge.empty());
}

RUDP_TEST(config_parsing_rejects_trailing_junk_overflow_and_bad_quotes) {
  auto check_throws = [](auto&& fn) {
    bool threw = false;
    try {
      fn();
    } catch (const std::exception&) {
      threw = true;
    }
    RUDP_CHECK(threw);
  };

  for (const char* bad_dur :
       {"", "1.2.3ms", "--5ms", "nan", "inf", "1e30s", "-5ms", "10years"}) {
    check_throws([&] { (void)parse_duration(bad_dur); });
  }

  for (const char* bad_toml : {
           "[gateway]\nport = 50000abc\n",
           "[scheduling]\nturn_byte_limit = 100xyz\n",
           "[gateway]\nport = 70000\n",
           "[gateway]\nport = -1\n",
           "[gateway]\nstream_id = 70000\n",
           "[gateway]\nstream_id = -5\n",
           "[gateway]\nbind_address = \"127.0.0.1\n",
           "[experiment]\nseed = -1\n",
           "[experiment]\nseed = 123abc\n",
       }) {
    check_throws([&] { (void)load_config_from_string(bad_toml); });
  }

  Config hex_cfg = load_config_from_string("[experiment]\nseed = 0x5EED\n");
  RUDP_CHECK_EQ(hex_cfg.seed, 24301ull);

  const Config base;
  {
    Config c = base;
    c.control_reserve_bytes = -1;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.control_reserve_bytes = 32;  // < 64
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.srtt_initial = 0;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.rttvar_initial = -1;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.bind_address.clear();
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.media_payload_bytes = std::numeric_limits<int>::max();
    c.max_frame_packets = 2;
    RUDP_CHECK(!validate_config(c).empty());
  }
  {
    Config c = base;
    c.max_sessions = std::numeric_limits<int>::max();
    RUDP_CHECK(!validate_config(c).empty());
  }
}

RUDP_TEST(virtual_network_full_loss_with_large_burst_drops_all) {
  VirtualClock clock;
  VirtualNetwork net(clock, 12345);
  const Endpoint src{1, 1};
  const Endpoint dst{2, 2};
  DirectionFaults df;
  df.data_loss = 1.0;
  df.burst_length = 200;
  net.set_route(src, df);

  for (int i = 0; i < 50; ++i) {
    Datagram dg;
    dg.peer = dst;
    dg.bytes.assign(kHeaderSize, std::byte{0});
    dg.bytes[0] = std::byte{kWireVersion};
    dg.bytes[1] = static_cast<std::byte>(PacketType::DATA);
    net.deliver_from(src, dg);
  }
  RUDP_CHECK_EQ(net.stats().dropped, 50u);
  RUDP_CHECK_EQ(net.in_flight(), 0u);

  // Unattached SimDriver must fail-safe without null-pointer dereference.
  SimDriver detached(clock);
  detached.run(ns_from_ms(10));
  RUDP_CHECK(detached.quiescent(ns_from_ms(20)));
}

}  // namespace rudp_test
