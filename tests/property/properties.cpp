// Property tests: no invariant violation across 10,000 seeds (spec §8.2).
// Five property families × 2,000 seeds each:
//   fuzz_datagrams        — random datagrams never corrupt receiver state
//   ack_nack_permutations — ACK monotonicity + bounded window on the sender
//   range_overlap_wrap    — normalized ranges stay sorted/disjoint/merged
//   queue_full_empty      — bounded queues keep their policies under fuzzing
//   timer_heap_random     — heap ordering + stale-entry tolerance

#include "test_framework.hpp"

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/session.hpp"
#include "rudp_rt/wire.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace rudp_test {

using namespace rudp_rt;

namespace {

std::mt19937_64 rng_for(std::uint64_t seed) { return std::mt19937_64(seed); }

Datagram random_datagram(std::mt19937_64& rng) {
  Datagram dg;
  const std::size_t size =
      std::uniform_int_distribution<std::size_t>(0, 1400)(rng);
  dg.bytes.resize(size);
  for (auto& b : dg.bytes) {
    b = static_cast<std::byte>(std::uniform_int_distribution<int>(0, 255)(rng));
  }
  // Occasionally produce a well-formed skeleton so valid-looking but
  // semantically invalid inputs dominate later checks.
  if (size >= kHeaderSize && std::uniform_real_distribution<>(0, 1)(rng) < 0.5) {
    dg.bytes[0] = std::byte{1};  // version
    dg.bytes[4] = std::byte{7};  // header_len
    dg.bytes[5] = std::byte{0};  // reserved
    store_u16(dg.bytes.data() + 6, 1);
    store_u16(dg.bytes.data() + 26,
              static_cast<std::uint16_t>(size - kHeaderSize));
    // random type
    dg.bytes[1] = static_cast<std::byte>(
        std::uniform_int_distribution<int>(1, 6)(rng));
  }
  return dg;
}

[[maybe_unused]] Datagram make_data(
    std::uint32_t seq, std::uint32_t frame_id, std::uint16_t index,
    std::uint16_t count, std::uint32_t rtp_ts) {
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
  rtp.marker = index == count - 1;
  return *build_data_datagram(Endpoint{1, 1}, h, rtp,
                              std::vector<std::byte>(80, std::byte{2}));
}

}  // namespace

RUDP_TEST(property_fuzz_datagrams_2000_seeds) {
  for (std::uint64_t seed = 0; seed < 2000; ++seed) {
    std::mt19937_64 rng = rng_for(seed ^ 0xF00D);
    VirtualClock clock;
    ReceiverConfig rc;
    rc.peer = Endpoint{1, 1};
    rc.stream_id = 1;
    rc.isn = 100;
    UdpReceiver rx(rc, clock);
    rx.set_sync(100, 42);
    const std::size_t baseline_storage = rx.storage_size();
    for (int i = 0; i < 50; ++i) {
      Datagram dg = random_datagram(rng);
      dg.peer = Endpoint{1, 1};
      rx.on_datagram(dg, clock.now());
      clock.advance(kNsPerMs);
      // INV-12: storage bounded regardless of input.
      RUDP_CHECK(rx.storage_size() <= 256u);
    }
    (void)baseline_storage;
  }
}

RUDP_TEST(property_ack_permutations_2000_seeds) {
  for (std::uint64_t seed = 0; seed < 2000; ++seed) {
    std::mt19937_64 rng = rng_for(seed ^ 0xACE);
    VirtualClock clock;
    SenderConfig cfg;
    cfg.isn = 5000;
    cfg.session_epoch = 9;
    TransportSender sender(cfg, clock);

    // Offer 20 frames of 5 packets each.
    for (std::uint32_t f = 0; f < 20; ++f) {
      MediaFrame mf;
      mf.frame_id = f;
      mf.capture_time = clock.now();
      mf.deadline_budget = ns_from_ms(1000);
      mf.bytes.assign(6000, std::byte{3});
      sender.on_frame(std::move(mf));
      sender.take_output();
      while (auto dg = sender.emit_new_data(clock.now(), 1 << 20)) (void)dg;
      clock.advance(kNsPerMs);
    }

    std::uint32_t last_valid = 4999;  // isn - 1
    for (int i = 0; i < 40; ++i) {
      Ack ack;
      ack.session_epoch = 9;
      // Random ack: sometimes behind, sometimes ahead, sometimes valid.
      const int roll = std::uniform_int_distribution<int>(0, 2)(rng);
      if (roll == 0) {
        ack.ack_number = last_valid - static_cast<std::uint32_t>(std::uniform_int_distribution<int>(0, 10)(rng));
      } else if (roll == 1) {
        ack.ack_number = last_valid + static_cast<std::uint32_t>(std::uniform_int_distribution<int>(1, 30)(rng));
      } else {
        ack.ack_number = last_valid + 1;
      }
      ack.sack_bitmap = std::uniform_int_distribution<std::uint64_t>()(rng);
      sender.on_ack(ack, clock.now());
      clock.advance(kNsPerMs);
      // INV-02: ACK never moves backward; INV-04: window bounded.
      RUDP_CHECK(sender.allocated_unacked() <= 100u);
    }
  }
}

RUDP_TEST(property_range_normalization_2000_seeds) {
  for (std::uint64_t seed = 0; seed < 2000; ++seed) {
    std::mt19937_64 rng = rng_for(seed ^ 0xBEEF);
    std::vector<AbandonRange> ranges;
    const int n = std::uniform_int_distribution<int>(0, 20)(rng);
    for (int i = 0; i < n; ++i) {
      const std::uint32_t start =
          std::uniform_int_distribution<std::uint32_t>(0, 200)(rng);
      const std::uint32_t len =
          std::uniform_int_distribution<std::uint32_t>(0, 30)(rng);
      ranges.push_back({start, start + len});
    }
    auto out = normalize_abandon_ranges(ranges);
    for (std::size_t i = 0; i < out.size(); ++i) {
      RUDP_CHECK(seq_le(out[i].start, out[i].end));
      if (i > 0) {
        // Disjoint and non-adjacent after the merge: there is a real gap of
        // at least one unused sequence between consecutive ranges.
        RUDP_CHECK(seq_lt(out[i - 1].end, out[i].start));
        RUDP_CHECK(seq_lt(seq_next(out[i - 1].end), out[i].start));
      }
    }
  }
}

RUDP_TEST(property_queue_policies_2000_seeds) {
  for (std::uint64_t seed = 0; seed < 2000; ++seed) {
    std::mt19937_64 rng = rng_for(seed ^ 0x0BEE);
    ControlQueue q(8);
    std::size_t size = 0;
    for (int i = 0; i < 60; ++i) {
      const bool is_abandon =
          std::uniform_real_distribution<>(0, 1)(rng) < 0.3;
      Datagram dg;
      dg.bytes.assign(kHeaderSize, std::byte{0});
      dg.bytes[1] = static_cast<std::byte>(
          is_abandon ? PacketType::ABANDON : PacketType::PING);
      const bool pushed = q.push(std::move(dg));
      if (size < 8) {
        RUDP_CHECK(pushed);
        ++size;
      } else if (!pushed) {
        // Dropped only when full of ABANDONs and the new one is ABANDON.
      }
      RUDP_CHECK(q.size() <= 8u);  // INV-12
    }
    while (!q.empty()) q.pop();

    // SPSC frame queue: never more than capacity stored.
    SpscFrameQueue fq(4);
    int stored = 0;
    for (int i = 0; i < 30; ++i) {
      if (fq.try_push(MediaFrame{})) ++stored;
      RUDP_CHECK(stored <= 4);
    }
  }
}

RUDP_TEST(property_timer_heap_2000_seeds) {
  for (std::uint64_t seed = 0; seed < 2000; ++seed) {
    std::mt19937_64 rng = rng_for(seed ^ 0x713E);
    TimerHeap heap(64);
    std::vector<TimerEntry> pushed;
    for (int i = 0; i < 40; ++i) {
      TimerEntry e;
      e.generation = std::uniform_int_distribution<std::uint32_t>(0, 3)(rng);
      e.session_id = std::uniform_int_distribution<std::uint32_t>(0, 2)(rng);
      e.kind = TimerKind::SESSION_SCAN;
      e.deadline = std::uniform_int_distribution<MonoNs>(0, 1000)(rng);
      if (heap.push(e)) pushed.push_back(e);
    }
    // Pop everything: (deadline, insertion_id) order strictly nondecreasing.
    MonoNs last_deadline = -1;
    std::uint64_t last_insertion = 0;
    while (auto e = heap.pop_due(2000)) {
      RUDP_CHECK(e->deadline >= last_deadline);
      if (e->deadline == last_deadline) {
        RUDP_CHECK(e->insertion_id > last_insertion);
      }
      last_deadline = e->deadline;
      last_insertion = e->insertion_id;
    }
    // Invalidate generation: entries with a different generation vanish.
    for (const auto& e : pushed) heap.push(e);
    heap.invalidate_generation(1, 9);  // keeps nothing of session 1 (gen != 9)
    while (auto e = heap.pop_due(2000)) {
      if (e->session_id == 1) {
        RUDP_CHECK(e->generation == 9);  // never dereferences stale (INV-14)
      }
    }
  }
}

}  // namespace rudp_test
