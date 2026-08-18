// reactor.cpp — worker supervision shared by the Gateway and Client
// runtimes: Capture Worker, Metrics Worker (spec §3.1: every worker has a
// stop event, a failure path, and a bounded join).

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <signal.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// CaptureWorker
// ---------------------------------------------------------------------------

CaptureWorker::CaptureWorker(IFrameSource& source, SpscFrameQueue& queue,
                             EventFd& wake, IClock& clock,
                             std::function<void()> on_drop)
    : source_(source), queue_(queue), wake_(wake), clock_(clock),
      on_drop_(std::move(on_drop)) {}

CaptureWorker::~CaptureWorker() {
  stop();
  join();
}

void CaptureWorker::start() {
  thread_ = std::thread([this] { run(); });
}

void CaptureWorker::stop() { stop_.store(true, std::memory_order_release); }

void CaptureWorker::join() {
  if (thread_.joinable()) thread_.join();
}

void CaptureWorker::run() {
  while (!stop_.load(std::memory_order_acquire)) {
    const MonoNs now = clock_.now();
    auto frame = source_.next(now);
    if (frame) {
      if (queue_.try_push(std::move(*frame))) {
        wake_.notify();
      } else {
        // Producer-side drop-newest (ADR-0001); counted via the documented
        // atomic callback (source ledger, spec §3.1).
        if (on_drop_) on_drop_();
      }
      continue;
    }
    // Bounded poll: the source paces itself via next(now); a 10 ms ceiling
    // keeps stop latency bounded (INV-17). This is capture pacing, not
    // protocol timing (spec §9 forbids sleep() only for protocol timing).
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 10 * 1000 * 1000;
    if (ts.tv_nsec >= static_cast<long>(kNsPerSec)) {
      ts.tv_sec += 1;
      ts.tv_nsec -= static_cast<long>(kNsPerSec);
    }
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
  }
}

// ---------------------------------------------------------------------------
// MetricsWorker
// ---------------------------------------------------------------------------

MetricsWorker::MetricsWorker(SpscSnapshotQueue& queue, std::string output_path)
    : queue_(queue), output_path_(std::move(output_path)) {}

MetricsWorker::~MetricsWorker() {
  stop();
  join();
}

void MetricsWorker::start() {
  thread_ = std::thread([this] { run(); });
}

void MetricsWorker::stop() { stop_.store(true, std::memory_order_release); }

void MetricsWorker::join() {
  if (thread_.joinable()) thread_.join();
}

void MetricsWorker::run() {
  std::ofstream out(output_path_, std::ios::app);
  while (true) {
    MetricsSnapshot s;
    bool got = queue_.try_pop(s);
    if (!got) {
      if (stop_.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const bool final = s.is_final;
    if (out.is_open()) {
      out << snapshot_to_json(s) << "\n";
      out.flush();
    }
    if (final) {
      final_acked_.store(true, std::memory_order_release);
      // Keep draining until stop; the Reactor joins after acking.
    }
  }
  // Persist anything that arrived after the stop flag.
  MetricsSnapshot s;
  while (queue_.try_pop(s)) {
    if (out.is_open()) {
      out << snapshot_to_json(s) << "\n";
      out.flush();
    }
    if (s.is_final) final_acked_.store(true, std::memory_order_release);
  }
}

}  // namespace rudp_rt
