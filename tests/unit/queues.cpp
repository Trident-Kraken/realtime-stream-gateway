// queues.cpp — bounded queue policies (ADR-0001/0007) and the timer heap
// (ADR-0010).

#include "test_framework.hpp"

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/session.hpp"

#include <algorithm>
#include <atomic>
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
  // The final snapshot replaced the oldest non-final in place (spec §3.3)
  // and is preserved for the consumer (INV-19).
  RUDP_CHECK(out1.is_final || out2.is_final);
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

}  // namespace rudp_test
