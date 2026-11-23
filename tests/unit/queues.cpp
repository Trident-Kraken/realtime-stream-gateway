// queues.cpp — bounded queue policies (ADR-0001/0007) and the timer heap
// (ADR-0010).

#include "test_framework.hpp"

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/session.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace rudp_test {

using namespace rudp_rt;

RUDP_TEST(spsc_frame_queue_drop_newest) {
  // Producer-side drop-newest: a full queue rejects the NEW frame and never
  // disturbs stored slots (ADR-0001, INV-13).
  SpscFrameQueue q(4);
  int produced = 0;
  for (int i = 0; i < 10; ++i) {
    MediaFrame f;
    f.frame_id = static_cast<std::uint32_t>(i);
    if (q.try_push(std::move(f))) ++produced;
  }
  RUDP_CHECK_EQ(produced, 4);  // frames 0..3 stored; 4..9 dropped by producer
  for (std::uint32_t expect = 0; expect < 4; ++expect) {
    MediaFrame out;
    RUDP_CHECK(q.try_pop(out));
    RUDP_CHECK_EQ(out.frame_id, expect);  // oldest preserved, never overwritten
  }
  MediaFrame none;
  RUDP_CHECK(!q.try_pop(none));
}

RUDP_TEST(spsc_frame_queue_cross_thread) {
  // INV-13: publish-after-write with acquire/release; consumer sees each
  // frame at most once and never a torn slot.
  SpscFrameQueue q(64);
  std::atomic<bool> done{false};
  std::atomic<long> sum{0};
  std::thread producer([&] {
    for (int i = 1; i <= 10000; ++i) {
      while (!q.try_push([&] {
        MediaFrame f;
        f.frame_id = static_cast<std::uint32_t>(i);
        return f;
      }())) {
        std::this_thread::yield();  // full: producer drop-newest in production
      }
    }
    done.store(true);
  });
  std::thread consumer([&] {
    while (!done.load() || q.capacity() > 0) {
      MediaFrame f;
      if (q.try_pop(f)) {
        sum.fetch_add(static_cast<long>(f.frame_id));
      } else if (done.load()) {
        MediaFrame last;
        if (!q.try_pop(last)) break;
      }
    }
  });
  producer.join();
  consumer.join();
  RUDP_CHECK_EQ(sum.load(), 10000L * 10001L / 2);
}

RUDP_TEST(control_queue_preserves_abandon) {
  PacketCounters counters;
  ControlQueue q(4);

  auto make = [](PacketType t) {
    Datagram dg;
    dg.bytes.assign(kHeaderSize, std::byte{0});
    dg.bytes[1] = static_cast<std::byte>(t);
    return dg;
  };

  // Fill with non-critical control.
  for (int i = 0; i < 4; ++i) {
    RUDP_CHECK(q.push(make(PacketType::PING)));
  }
  // ABANDON evicts the oldest non-critical entry (preserved, spec §3.3).
  RUDP_CHECK(q.push(make(PacketType::ABANDON)));
  RUDP_CHECK_EQ(q.size(), 4u);
  RUDP_CHECK_EQ(static_cast<PacketType>(q.front().bytes[1]),
                PacketType::PING);  // second PING survived; first was evicted
  while (!q.empty()) q.pop();

  // Full of ABANDON: the bounded cap wins; newest ABANDON is dropped.
  for (int i = 0; i < 4; ++i) RUDP_CHECK(q.push(make(PacketType::ABANDON)));
  RUDP_CHECK(!q.push(make(PacketType::ABANDON)));
  RUDP_CHECK_EQ(q.size(), 4u);
}

RUDP_TEST(snapshot_queue_final_never_dropped) {
  SpscSnapshotQueue q(2);
  MetricsSnapshot a, b;
  RUDP_CHECK(q.try_push(std::move(a)));
  RUDP_CHECK(q.try_push(std::move(b)));
  // Overflow: non-final dropped and counted.
  MetricsSnapshot c;
  RUDP_CHECK(!q.try_push(std::move(c)));
  RUDP_CHECK_EQ(q.dropped_non_final(), 1u);
  // Final replaces the oldest non-final entries.
  MetricsSnapshot f;
  f.is_final = true;
  RUDP_CHECK(q.try_push(std::move(f)));
  MetricsSnapshot out1, out2;
  RUDP_CHECK(q.try_pop(out1));
  RUDP_CHECK(q.try_pop(out2));
  // The final snapshot replaced the oldest non-final in order (spec §3.3)
  // and is preserved for the consumer (INV-19) without producer-side races.
  RUDP_CHECK(!out1.is_final);
  RUDP_CHECK(out2.is_final);

  // Concurrent producer/consumer stress under overflow + final publication.
  SpscSnapshotQueue cq(4);
  std::atomic<bool> saw_final{false};
  std::thread consumer([&] {
    while (!saw_final.load(std::memory_order_relaxed)) {
      MetricsSnapshot s;
      if (cq.try_pop(s)) {
        if (s.is_final) saw_final.store(true, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (int i = 0; i < 200; ++i) {
    MetricsSnapshot snap;
    snap.taken_at = i;
    snap.clock_mode = "virtual";
    cq.try_push(std::move(snap));
  }
  MetricsSnapshot fin;
  fin.is_final = true;
  fin.taken_at = 999;
  while (!cq.try_push(fin)) {
    std::this_thread::yield();
  }
  consumer.join();
  RUDP_CHECK(saw_final.load());
}

RUDP_TEST(timer_heap_ordering_and_tiebreak) {
  TimerHeap heap(16);
  // Same deadline: kind priority then insertion order.
  RUDP_CHECK(heap.push({1, 0, TimerKind::WATCHDOG, 100, 0}));
  RUDP_CHECK(heap.push({1, 0, TimerKind::KEEPALIVE, 100, 0}));
  RUDP_CHECK(heap.push({1, 0, TimerKind::SESSION_TIMEOUT, 100, 0}));
  RUDP_CHECK(heap.push({1, 0, TimerKind::SHUTDOWN, 100, 0}));
  RUDP_CHECK(heap.push({2, 0, TimerKind::SESSION_SCAN, 50, 0}));
  RUDP_CHECK(heap.push({1, 0, TimerKind::SESSION_SCAN, 200, 0}));

  auto e = heap.pop_due(0);
  RUDP_CHECK(!e.has_value());
  e = heap.pop_due(100);
  RUDP_CHECK(e.has_value());
  RUDP_CHECK(e->kind == TimerKind::SESSION_SCAN && e->generation == 2);
  e = heap.pop_due(100);
  RUDP_CHECK(e->kind == TimerKind::SHUTDOWN);  // priority 0
  e = heap.pop_due(100);
  RUDP_CHECK(e->kind == TimerKind::SESSION_TIMEOUT);
  e = heap.pop_due(100);
  RUDP_CHECK(e->kind == TimerKind::KEEPALIVE);
  e = heap.pop_due(100);
  RUDP_CHECK(e->kind == TimerKind::WATCHDOG);
  e = heap.pop_due(100);
  RUDP_CHECK(!e.has_value());  // 200 not due yet
  e = heap.pop_due(200);
  RUDP_CHECK(e.has_value() && e->deadline == 200);
}

RUDP_TEST(timer_heap_capacity_and_invalidation) {
  TimerHeap heap(3);
  RUDP_CHECK(heap.push({1, 1, TimerKind::SESSION_SCAN, 10, 0}));
  RUDP_CHECK(heap.push({1, 1, TimerKind::SESSION_SCAN, 20, 0}));
  RUDP_CHECK(heap.push({1, 1, TimerKind::SESSION_SCAN, 30, 0}));
  RUDP_CHECK(!heap.push({1, 1, TimerKind::SESSION_SCAN, 40, 0}));  // exhausted

  // Invalidate stale generation: only matching-generation entries survive.
  heap.invalidate_generation(1, 7);
  RUDP_CHECK_EQ(heap.size(), 0u);
}

RUDP_TEST(timer_heap_replace_session_timer_prevents_ratchet) {
  TimerHeap heap(2);
  for (MonoNs d = 1000; d >= 100; d -= 100) {
    RUDP_CHECK(heap.replace_session_timer({1, 1, TimerKind::SESSION_SCAN, d, 0}));
    RUDP_CHECK_EQ(heap.size(), 1u);
    RUDP_CHECK_EQ(heap.high_water(), 1u);
    RUDP_CHECK(heap.peek().has_value());
    RUDP_CHECK_EQ(heap.peek()->deadline, d);
  }

  RUDP_CHECK(heap.replace_session_timer({1, 2, TimerKind::SESSION_SCAN, 500, 0}));
  RUDP_CHECK_EQ(heap.size(), 2u);

  // Replacing an existing session's timer must succeed even at full capacity.
  RUDP_CHECK(heap.replace_session_timer({1, 1, TimerKind::SESSION_SCAN, 50, 0}));
  RUDP_CHECK_EQ(heap.size(), 2u);
  RUDP_CHECK_EQ(heap.peek()->deadline, 50);

  // Adding a distinct 3rd session at full capacity must still be rejected.
  RUDP_CHECK(!heap.replace_session_timer({1, 3, TimerKind::SESSION_SCAN, 200, 0}));
  RUDP_CHECK_EQ(heap.size(), 2u);

  heap.erase_session(1);
  RUDP_CHECK_EQ(heap.size(), 1u);
  RUDP_CHECK_EQ(heap.peek()->session_id, 2u);
}

RUDP_TEST(snapshot_queue_eviction_preserves_order_and_final_slot_sync) {
  auto make_snap = [](MonoNs ts, bool fin) {
    MetricsSnapshot s;
    s.taken_at = ts;
    s.is_final = fin;
    s.clock_mode = "virtual";
    return s;
  };

  // Ring [f1(1), f2(2), nf3(3)] + overflow f4(4): evicting nf3 at index 2 must
  // shift [f1, f2] forward so consumer reads [1, 2, 4] in chronological order.
  SpscSnapshotQueue q(3);
  RUDP_CHECK(q.try_push(make_snap(1, true)));
  RUDP_CHECK(q.try_push(make_snap(2, true)));
  RUDP_CHECK(q.try_push(make_snap(3, false)));
  RUDP_CHECK(q.try_push(make_snap(4, true)));

  MetricsSnapshot o1, o2, o3, extra;
  RUDP_CHECK(q.try_pop(o1));
  RUDP_CHECK(q.try_pop(o2));
  RUDP_CHECK(q.try_pop(o3));
  RUDP_CHECK(!q.try_pop(extra));
  RUDP_CHECK_EQ(o1.taken_at, 1);
  RUDP_CHECK_EQ(o2.taken_at, 2);
  RUDP_CHECK_EQ(o3.taken_at, 4);
  RUDP_CHECK_EQ(q.dropped_non_final(), 1u);

  // Full-of-finals overflow must not miscount a non-final drop.
  SpscSnapshotQueue qfull(2);
  RUDP_CHECK(qfull.try_push(make_snap(10, true)));
  RUDP_CHECK(qfull.try_push(make_snap(20, true)));
  RUDP_CHECK(qfull.try_push(make_snap(30, true)));
  MetricsSnapshot p1, p2, p3;
  RUDP_CHECK(qfull.try_pop(p1));
  RUDP_CHECK(qfull.try_pop(p2));
  RUDP_CHECK(qfull.try_pop(p3));
  RUDP_CHECK_EQ(p1.taken_at, 10);
  RUDP_CHECK_EQ(p2.taken_at, 20);
  RUDP_CHECK_EQ(p3.taken_at, 30);
  RUDP_CHECK_EQ(qfull.dropped_non_final(), 0u);

  // Concurrent multi-cycle overflow_final_ publication stress test.
  SpscSnapshotQueue cq(3);
  constexpr int kFinalTarget = 40;
  std::atomic<int> finals_seen{0};
  std::thread consumer([&] {
    while (finals_seen.load(std::memory_order_relaxed) < kFinalTarget) {
      MetricsSnapshot s;
      if (cq.try_pop(s)) {
        if (s.is_final) finals_seen.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  });
  for (int fin_idx = 0; fin_idx < kFinalTarget; ++fin_idx) {
    for (int i = 0; i < 6; ++i) {
      cq.try_push(make_snap(fin_idx * 10 + i, false));
    }
    while (!cq.try_push(make_snap(10000 + fin_idx, true))) {
      std::this_thread::yield();
    }
  }
  consumer.join();
  RUDP_CHECK_EQ(finals_seen.load(), kFinalTarget);
}

RUDP_TEST(token_bucket_refill_no_overflow_and_preserves_subtoken_progress) {
  // 1) Default 4 MiB/s rate over 3600s idle (and huge idle / high rate) must
  //    not overflow signed int64.
  TokenBucket tb;
  tb.configure(4 * 1024 * 1024, 65536);
  tb.refill(0);
  RUDP_CHECK(tb.try_take(65536));
  tb.refill(kNsPerSec * 3600);
  RUDP_CHECK_EQ(tb.tokens(), 65536);

  RUDP_CHECK(tb.try_take(65536));
  tb.refill(kNsPerSec * 100000);
  RUDP_CHECK_EQ(tb.tokens(), 65536);

  tb.configure(1'000'000'000'000LL, 65536);
  tb.refill(0);
  RUDP_CHECK(tb.try_take(65536));
  tb.refill(ns_from_ms(1));
  RUDP_CHECK_EQ(tb.tokens(), 65536);

  // 2) Sub-token polling (1 ms ticks at 100 tokens/s = 0.1 tokens/tick) must
  //    accumulate remainder nanoseconds instead of starving at 0.
  TokenBucket rl;
  rl.configure(100, 32);
  rl.refill(0);
  RUDP_CHECK(rl.try_take(32));
  int taken = 0;
  for (int ms = 1; ms <= 1000; ++ms) {
    rl.refill(ns_from_ms(ms));
    if (rl.try_take(1)) ++taken;
  }
  RUDP_CHECK_EQ(taken, 100);
}

RUDP_TEST(metrics_worker_single_newline_and_worker_failures) {
  const std::string path = "/tmp/rudp_mw_single_newline_test.jsonl";
  std::remove(path.c_str());

  SpscSnapshotQueue sq(4);
  MetricsWorker mw(sq, path);
  mw.start();

  MetricsSnapshot s1;
  s1.taken_at = 10;
  s1.is_final = false;
  s1.clock_mode = "virtual";
  RUDP_CHECK(sq.try_push(s1));

  MetricsSnapshot s2;
  s2.taken_at = 20;
  s2.is_final = true;
  s2.clock_mode = "virtual";
  RUDP_CHECK(sq.try_push(s2));

  for (int i = 0; i < 200 && !mw.final_acked(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  RUDP_CHECK(mw.final_acked());
  RUDP_CHECK(!mw.failed());
  mw.stop();
  mw.join();

  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string raw = ss.str();
  std::remove(path.c_str());
  // JSONL must contain one newline per record and no blank lines (\n\n).
  RUDP_CHECK(raw.find("\n\n") == std::string::npos);

  // Unwritable output path: MetricsWorker flags failed() and still acks final.
  SpscSnapshotQueue bad_sq(2);
  MetricsWorker bad_mw(bad_sq, "/nonexistent_dir_rudp_xyz/out.jsonl");
  bad_mw.start();
  MetricsSnapshot fin;
  fin.taken_at = 30;
  fin.is_final = true;
  fin.clock_mode = "virtual";
  RUDP_CHECK(bad_sq.try_push(fin));
  for (int i = 0; i < 200 && !bad_mw.final_acked(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  RUDP_CHECK(bad_mw.final_acked());
  RUDP_CHECK(bad_mw.failed());
  bad_mw.stop();
  bad_mw.join();

  // Throwing IFrameSource: CaptureWorker catches cleanly and sets failed().
  struct ThrowingSource final : IFrameSource {
    std::optional<MediaFrame> next(MonoNs) override {
      throw std::runtime_error("camera failed");
    }
  };
  ThrowingSource bad_src;
  SpscFrameQueue fq(2);
  EventFd wake;
  VirtualClock vclk;
  CaptureWorker cw(bad_src, fq, wake, vclk, nullptr);
  cw.start();
  for (int i = 0; i < 100 && !cw.failed(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  RUDP_CHECK(cw.failed());
  cw.stop();
  cw.join();
}

}  // namespace rudp_test
