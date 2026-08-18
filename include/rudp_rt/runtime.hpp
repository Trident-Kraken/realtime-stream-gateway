#pragma once
// rudp_rt/runtime.hpp — Linux runtime: the EventLoop reactor shell, SPSC
// queues, workers, and the Gateway/Client process lifecycles (spec §3,
// phase 4).
//
// Thread and ownership rules (spec §3.1):
//   - the EventLoop thread is the sole protocol-state writer;
//   - Capture Worker and Metrics Worker communicate only via SPSC queues
//     plus eventfd wakeups; no atomics touch protocol state;
//   - every worker has a stop flag, a failure path, and a bounded join;
//   - timer entries carry (generation, session_id, kind, deadline,
//     insertion_id); stale entries are ignored (ADR-0010).

#include "rudp_rt/config.hpp"
#include "rudp_rt/event_loop.hpp"
#include "rudp_rt/metrics.hpp"
#include "rudp_rt/network.hpp"
#include "rudp_rt/session.hpp"
#include "rudp_rt/types.hpp"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Bounded SPSC frame queue (spec §3.3; ADR-0001: producer drop-newest).
// Producer: Capture Worker. Consumer: the EventLoop thread.
// ---------------------------------------------------------------------------
class SpscFrameQueue {
 public:
  explicit SpscFrameQueue(std::size_t capacity)
      : slots_(capacity), capacity_(capacity) {}

  // Producer side. Returns false (and leaves the queue untouched) when full;
  // the producer counts capture_dropped and continues.
  bool try_push(MediaFrame&& frame) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail == capacity_) return false;
    slots_[head % capacity_] = std::move(frame);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  // Consumer side.
  bool try_pop(MediaFrame& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = std::move(slots_[tail % capacity_]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::size_t capacity() const { return capacity_; }

 private:
  std::vector<MediaFrame> slots_;
  std::size_t capacity_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
};

// Bounded SPSC queue for immutable metrics snapshots (final never dropped).
class SpscSnapshotQueue {
 public:
  explicit SpscSnapshotQueue(std::size_t capacity)
      : items_(capacity), capacity_(capacity) {}

  bool try_push(MetricsSnapshot s) {  // by value; final never dropped
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head - tail == capacity_) {
      if (s.is_final) {
        // Replace the oldest non-final snapshot IN PLACE; the final snapshot
        // is never dropped (spec §3.3, INV-19).
        for (std::size_t i = tail; i < head; ++i) {
          auto& slot = items_[i % capacity_];
          if (!slot.is_final) {
            slot = std::move(s);
            dropped_non_final_++;
            return true;
          }
        }
        return false;  // only final snapshots queued (nothing to replace)
      }
      dropped_non_final_++;
      return false;
    }
    items_[head % capacity_] = std::move(s);
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(MetricsSnapshot& out) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail == head) return false;
    out = std::move(items_[tail % capacity_]);
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  std::uint64_t dropped_non_final() const {
    return dropped_non_final_.load(std::memory_order_relaxed);
  }

 private:
  std::vector<MetricsSnapshot> items_;
  std::size_t capacity_;
  alignas(64) std::atomic<std::size_t> head_{0};
  alignas(64) std::atomic<std::size_t> tail_{0};
  std::atomic<std::uint64_t> dropped_non_final_{0};
};

// ---------------------------------------------------------------------------
// SocketNetwork — INetwork over one non-blocking UDP socket (ADR-0009).
// Owns the fd; binds/validates socket buffers at construction.
// ---------------------------------------------------------------------------
class SocketNetwork final : public INetwork {
 public:
  struct Options {
    Endpoint bind;          // ipv4_be + port_be; port 0 = ephemeral
    bool connect_peer = false;  // connect() for error feedback (client)
    std::optional<Endpoint> peer;
    int so_rcvbuf = 0;
    int so_sndbuf = 0;
    int so_rcvbuf_min = 0;
    int so_sndbuf_min = 0;
  };

  // Throws std::runtime_error on failure (startup transaction, spec §5).
  explicit SocketNetwork(const Options& opts);
  ~SocketNetwork() override;

  SocketNetwork(const SocketNetwork&) = delete;
  SocketNetwork& operator=(const SocketNetwork&) = delete;

  SendResult send(const Endpoint& peer, std::span<const std::byte> bytes) override;
  RecvResult receive(std::span<std::byte> buffer) override;

  int fd() const { return fd_; }
  Endpoint local() const { return local_; }
  int last_errno() const { return last_errno_; }

  // Returns the granted SO_RCVBUF/SO_SNDBUF (post-kernel-doubling values).
  int granted_rcvbuf() const { return granted_rcvbuf_; }
  int granted_sndbuf() const { return granted_sndbuf_; }

 private:
  int fd_ = -1;
  Endpoint local_{};
  int last_errno_ = 0;
  int granted_rcvbuf_ = 0;
  int granted_sndbuf_ = 0;
  bool connected_ = false;
};

// ---------------------------------------------------------------------------
// Capture Worker (spec §3.1). Runs the injected source on its own thread,
// publishes to the SPSC queue, wakes the EventLoop.
// ---------------------------------------------------------------------------
class CaptureWorker {
 public:
  CaptureWorker(IFrameSource& source, SpscFrameQueue& queue, EventFd& wake,
                IClock& clock, std::function<void()> on_drop);
  ~CaptureWorker();

  void start();
  void stop();          // sets the stop flag; join() bounded below
  void join();
  bool failed() const { return failed_.load(); }

 private:
  void run();

  IFrameSource& source_;
  SpscFrameQueue& queue_;
  EventFd& wake_;
  IClock& clock_;
  std::function<void()> on_drop_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> failed_{false};
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// Metrics Worker. Consumes immutable snapshots; persists them; the final
// snapshot is never dropped and its handling is acknowledged before exit.
// ---------------------------------------------------------------------------
class MetricsWorker {
 public:
  MetricsWorker(SpscSnapshotQueue& queue, std::string output_path);
  ~MetricsWorker();

  void start();
  void stop();
  void join();
  bool final_acked() const { return final_acked_.load(); }

 private:
  void run();

  SpscSnapshotQueue& queue_;
  std::string output_path_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> final_acked_{false};
  std::thread thread_;
};

// ---------------------------------------------------------------------------
// Gateway process runtime (Phase 4). Owns the socket, workers, and the
// GatewayNode protocol core; the EventLoop drives it through its Delegate.
// ---------------------------------------------------------------------------
class Gateway final : private EventLoop::Delegate {
 public:
  explicit Gateway(Config cfg);
  ~Gateway();

  // Startup transaction (spec Phase 4): validate config, create socket +
  // eventfds + source, start workers. Throws std::runtime_error with the
  // failing step on any error (rollback).
  void start();

  // Runs the EventLoop until stop_signal/stop() or duration elapses,
  // including the bounded graceful drain (INV-17/INV-18). Returns the final
  // snapshot (also handed to the MetricsWorker).
  MetricsSnapshot run(MonoNs duration);

  void stop();   // thread-safe: wakes the EventLoop
  void request_shutdown();  // same as stop() but records user intent
  bool metrics_final_acked() const;
  void set_metrics_path(std::string path) { metrics_path_ = std::move(path); }

 private:
  // EventLoop::Delegate — the gateway's policy over the shared machinery.
  void drain_input(MonoNs now) override;
  void on_timer(const TimerEntry& e, MonoNs now) override;
  bool pump(INetwork& net, MonoNs now) override;
  void after_wake(MonoNs now) override;
  void on_stop() override;
  bool finished() const override;
  int wait_timeout_ms() const override;
  void on_socket_error() override;

  MetricsSnapshot build_snapshot(bool final_snapshot);
  void drain_capture();

  Config cfg_;
  IClock& clock() { return *real_clock_; }
  SourceLedger source_ledger_;  // EventLoop-thread-owned (INV-10);
                                // capture_dropped arrives via a documented
                                // atomic (spec 3.1)

  std::unique_ptr<RealClock> real_clock_;
  std::unique_ptr<SocketNetwork> net_;
  std::unique_ptr<GatewayNode> node_;
  EventLoop loop_;
  SpscFrameQueue capture_queue_;
  SpscSnapshotQueue snapshot_queue_;
  std::unique_ptr<EventFd> capture_wake_;
  std::unique_ptr<EventFd> command_wake_;
  bool draining_ = false;
  MonoNs drain_deadline_ = 0;
  MonoNs next_metrics_ = 0;
  bool running_ = false;
  // One outstanding scan entry per live session (ADR-0010).
  std::map<std::uint32_t, std::pair<std::uint32_t, MonoNs>> scan_armed_;
  std::uint64_t mem_hwm_ = 0;
  std::uint64_t resource_exhausted_ = 0;
  std::uint64_t capture_hwm_ = 0;
  // Documented atomic (spec 3.1): Capture Worker -> EventLoop drop counter.
  std::atomic<std::uint64_t> capture_dropped_atomic_{0};

  std::unique_ptr<SyntheticSource> source_;
  std::unique_ptr<CaptureWorker> capture_;
  std::unique_ptr<MetricsWorker> metrics_;
  std::string metrics_path_ = "gateway_metrics.jsonl";
};

// ---------------------------------------------------------------------------
// Client process runtime (Phase 4).
// ---------------------------------------------------------------------------
class Client final : private EventLoop::Delegate {
 public:
  Client(Config cfg, Endpoint gateway, std::uint16_t stream_id);
  ~Client();

  void start();
  // Runs until duration elapses or stop(); returns final client metrics.
  MetricsSnapshot run(MonoNs duration);
  void stop();
  bool metrics_final_acked() const;
  void set_metrics_path(std::string path) { metrics_path_ = std::move(path); }

 private:
  // EventLoop::Delegate — the client's policy over the shared machinery.
  void drain_input(MonoNs now) override;
  void on_timer(const TimerEntry& e, MonoNs now) override;
  bool pump(INetwork& net, MonoNs now) override;
  void after_wake(MonoNs now) override;
  bool finished() const override;

  MetricsSnapshot build_snapshot(bool final_snapshot);

  Config cfg_;
  Endpoint gateway_{};
  std::uint16_t stream_id_ = 1;
  std::unique_ptr<RealClock> real_clock_;
  std::unique_ptr<SocketNetwork> net_;
  std::unique_ptr<ClientNode> node_;
  EventLoop loop_;
  SpscSnapshotQueue snapshot_queue_;
  std::unique_ptr<MetricsWorker> metrics_;
  std::unique_ptr<EventFd> command_wake_;
  bool running_ = false;
  bool scan_armed_ = false;
  std::string metrics_path_ = "client_metrics.jsonl";
};

}  // namespace rudp_rt
