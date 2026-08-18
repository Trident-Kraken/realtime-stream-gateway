// gateway.cpp — Gateway process runtime: startup transaction, EventLoop
// delegate policy, bounded graceful shutdown (Phase 4). The loop machinery
// (epoll/timerfd/signalfd/timer heap) lives in EventLoop; this file owns
// only gateway policy.

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/wire.hpp"

#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <cerrno>
#include <sys/socket.h>

namespace rudp_rt {

Gateway::Gateway(Config cfg)
    : cfg_(std::move(cfg)),
      loop_(cfg_.timer_capacity),
      capture_queue_(static_cast<std::size_t>(cfg_.capture_queue_frames)),
      snapshot_queue_(static_cast<std::size_t>(cfg_.metrics_queue_snapshots)) {
  auto errors = validate_config(cfg_);
  if (!errors.empty()) {
    std::string msg = "invalid configuration:";
    for (const auto& e : errors) msg += "\n  - " + e;
    throw std::runtime_error(msg);
  }
  real_clock_ = std::make_unique<RealClock>();
}

Gateway::~Gateway() {
  if (capture_) {
    capture_->stop();
    capture_->join();
  }
  if (metrics_) {
    metrics_->stop();
    metrics_->join();
  }
}

void Gateway::start() {
  SocketNetwork::Options opts;
  // Parse bind address (inet_pton writes network byte order into in_addr).
  in_addr parsed{};
  if (::inet_pton(AF_INET, cfg_.bind_address.c_str(), &parsed) != 1) {
    throw std::runtime_error("invalid bind_address: " + cfg_.bind_address);
  }
  opts.bind.ipv4_be = parsed.s_addr;
  opts.bind.port_be = htons(cfg_.port);
  opts.so_rcvbuf = cfg_.so_rcvbuf_request;
  opts.so_sndbuf = cfg_.so_sndbuf_request;
  opts.so_rcvbuf_min = cfg_.so_rcvbuf_min;
  opts.so_sndbuf_min = cfg_.so_sndbuf_min;
  net_ = std::make_unique<SocketNetwork>(opts);

  auto rng = [engine = std::make_shared<std::mt19937>(
                  std::random_device{}())]() { return (*engine)(); };
  node_ = std::make_unique<GatewayNode>(cfg_, *real_clock_, rng);

  capture_wake_ = std::make_unique<EventFd>();
  command_wake_ = std::make_unique<EventFd>();

  loop_.watch_socket(*net_, net_->fd());
  loop_.watch_stop_wake(*command_wake_);
  // Capture work itself is drained in drain_input(); the wake only marks
  // the queue non-empty.
  loop_.watch_fd(capture_wake_->fd(), [this] { capture_wake_->drain(); });

  SyntheticSource::Params sp;
  sp.stream_id = cfg_.stream_id;
  sp.ssrc = cfg_.ssrc ? cfg_.ssrc : (0x5EED0000u ^ cfg_.stream_id);
  sp.media_bytes = static_cast<std::uint32_t>(cfg_.frame_bytes);
  sp.fps = cfg_.fps;
  sp.deadline_budget = cfg_.deadline_budget;
  sp.start_time = clock().now();
  source_ = std::make_unique<SyntheticSource>(sp);
  node_->set_keyframe_request_callback([this] {
    if (source_) source_->request_keyframe();
  });

  metrics_ = std::make_unique<MetricsWorker>(snapshot_queue_, metrics_path_);
  metrics_->start();

  // Capture drop accounting crosses threads through the documented atomic
  // (spec §3.1); the EventLoop thread folds it into the source ledger at
  // snapshot.
  capture_ = std::make_unique<CaptureWorker>(
      *source_, capture_queue_, *capture_wake_, clock(), [this] {
        capture_dropped_atomic_.fetch_add(1, std::memory_order_relaxed);
      });
  capture_->start();

  running_ = true;
}

MetricsSnapshot Gateway::run(MonoNs duration) {
  if (!running_) {
    throw std::runtime_error("Gateway::run before start");
  }
  const MonoNs end_at = duration > 0 ? clock().now() + duration : 0;
  next_metrics_ = clock().now() + cfg_.metrics_interval;
  loop_.run(*this, clock(), end_at);

  if (capture_) {
    capture_->stop();
    capture_->join();
  }
  // Final snapshot after all terminal classification (INV-18/INV-19).
  MetricsSnapshot final_snapshot = build_snapshot(true);
  snapshot_queue_.try_push(final_snapshot);  // copies; final never dropped
  running_ = false;
  return final_snapshot;
}

void Gateway::stop() {
  if (command_wake_) command_wake_->notify();
}

void Gateway::request_shutdown() { stop(); }

// --- EventLoop::Delegate: the gateway's policy over the shared machinery ---

void Gateway::drain_input(MonoNs now) {
  // Stage 1/2: readable UDP drained to EAGAIN (level-triggered, §3.2).
  loop_.drain_socket(
      [this, now](Datagram&& dg) { node_->on_datagram(std::move(dg), now); },
      [this] { node_->counters().malformed++; });  // rejected before mutation
  // Stage 4: capture queue.
  drain_capture();
}

void Gateway::on_timer(const TimerEntry& e, MonoNs now) {
  // Stage 3: due timers (SESSION_SCAN entries).
  if (e.kind == TimerKind::SESSION_SCAN && e.session_id != 0) {
    node_->on_session_timer(e.session_id, e.generation, now);
    auto it = scan_armed_.find(e.session_id);
    if (it != scan_armed_.end() && it->second.first == e.generation) {
      scan_armed_.erase(it);
    }
  }
}

bool Gateway::pump(INetwork& net, MonoNs now) {
  // Stage 5: bounded writes; EPOLLOUT follows pending output.
  return node_->pump(net, now);
}

void Gateway::after_wake(MonoNs now) {
  // Stage 6: metrics snapshots.
  if (!draining_ && now >= next_metrics_) {
    snapshot_queue_.try_push(build_snapshot(false));
    next_metrics_ = now + cfg_.metrics_interval;
  }

  // Re-arm per-session scan entries (ADR-0010).
  for (const auto& [id, gen] : node_->live_sessions()) {
    const MonoNs deadline = node_->session_next_deadline(id);
    auto it = scan_armed_.find(id);
    if (it == scan_armed_.end()) {
      TimerEntry e;
      e.generation = gen;
      e.session_id = id;
      e.kind = TimerKind::SESSION_SCAN;
      e.deadline = deadline;
      if (loop_.push_timer(e)) {
        scan_armed_[id] = {gen, deadline};
      } else {
        resource_exhausted_++;
      }
    } else if (deadline < it->second.second) {
      TimerEntry e;
      e.generation = gen;
      e.session_id = id;
      e.kind = TimerKind::SESSION_SCAN;
      e.deadline = deadline;
      if (loop_.push_timer(e)) it->second = {gen, deadline};
    }
  }
}

void Gateway::on_stop() {
  // Graceful shutdown barrier (INV-18): no new admissions, in-flight frames
  // finalized, bounded drain (INV-17).
  draining_ = true;
  drain_deadline_ = clock().now() + cfg_.drain_deadline;
  node_->begin_shutdown(clock().now());
  if (capture_) {
    capture_->stop();
  }
}

bool Gateway::finished() const {
  return draining_ &&
         (real_clock_->now() >= drain_deadline_ || node_->output_drained());
}

int Gateway::wait_timeout_ms() const { return draining_ ? 1 : 20; }

void Gateway::on_socket_error() {
  // Unconnected UDP: ICMP feedback surfaces here; count it and clear the
  // error. Real teardown decisions happen on send results (ADR-0009), not
  // on transient EPOLLERR.
  int soerr = 0;
  socklen_t slen = sizeof(soerr);
  ::getsockopt(net_->fd(), SOL_SOCKET, SO_ERROR, &soerr, &slen);
  node_->counters().send_errors++;
}

void Gateway::drain_capture() {
  const MonoNs now = clock().now();
  MediaFrame frame;
  std::uint64_t popped = 0;
  while (capture_queue_.try_pop(frame)) {
    ++popped;
    source_ledger_.admitted();
    node_->offer_frame(std::move(frame), now);
  }
  capture_hwm_ = std::max<std::uint64_t>(capture_hwm_, popped);
  const std::uint64_t dropped =
      capture_dropped_atomic_.load(std::memory_order_relaxed);
  source_ledger_.fold_capture_drops(dropped);
  mem_hwm_ = std::max<std::uint64_t>(mem_hwm_, node_->memory_high_water());
}

bool Gateway::metrics_final_acked() const {
  return metrics_ ? metrics_->final_acked() : false;
}

MetricsSnapshot Gateway::build_snapshot(bool final_snapshot) {
  const MonoNs now = clock().now();
  MetricsSnapshot s;
  s.taken_at = now;
  s.is_final = final_snapshot;
  s.clock_mode = "real";
  s.seed = 0;
  s.source = source_ledger_;
  s.packets = node_->counters();
  s.watchdog_violations = node_->watchdog_violations();
  s.resource_exhausted = node_->resource_exhausted() + resource_exhausted_;
  s.memory_high_water_bytes = mem_hwm_;
  s.queue_depth_hwm_capture = static_cast<std::uint32_t>(capture_hwm_);
  s.queue_depth_hwm_control = static_cast<std::uint32_t>(node_->control_hwm());
  s.queue_depth_hwm_timer =
      static_cast<std::uint32_t>(loop_.timers().high_water());
  s.sessions = node_->collect_sessions();
  return s;
}

}  // namespace rudp_rt
