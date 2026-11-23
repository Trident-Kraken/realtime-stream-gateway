// client.cpp — Client process runtime: EventLoop delegate policy and bounded
// shutdown (Phase 4). The loop machinery lives in EventLoop; this file owns
// only client policy.

#include "rudp_rt/runtime.hpp"
#include "rudp_rt/wire.hpp"

#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace rudp_rt {

Client::Client(Config cfg, Endpoint gateway, std::uint16_t stream_id)
    : cfg_(std::move(cfg)),
      gateway_(gateway),
      stream_id_(stream_id),
      loop_(cfg_.timer_capacity),
      snapshot_queue_(static_cast<std::size_t>(cfg_.metrics_queue_snapshots)) {
  auto errors = validate_config(cfg_);
  if (!errors.empty()) {
    std::string msg = "invalid configuration:";
    for (const auto& e : errors) msg += "\n  - " + e;
    throw std::runtime_error(msg);
  }
  real_clock_ = std::make_unique<RealClock>();
}

Client::~Client() {
  if (metrics_) {
    metrics_->stop();
    metrics_->join();
  }
}

void Client::rollback() {
  if (metrics_) {
    metrics_->stop();
    metrics_->join();
    metrics_.reset();
  }
  command_wake_.reset();
  node_.reset();
  net_.reset();
  running_ = false;
}

void Client::start() {
  try {
    SocketNetwork::Options opts;
    opts.bind.port_be = 0;  // ephemeral local port
    opts.connect_peer = true;
    opts.peer = gateway_;
    opts.so_rcvbuf = cfg_.so_rcvbuf_request;
    opts.so_sndbuf = cfg_.so_sndbuf_request;
    opts.so_rcvbuf_min = cfg_.so_rcvbuf_min;
    opts.so_sndbuf_min = cfg_.so_sndbuf_min;
    net_ = std::make_unique<SocketNetwork>(opts);

    node_ = std::make_unique<ClientNode>(
        cfg_, *real_clock_, gateway_, stream_id_,
        [engine = std::make_shared<std::mt19937>(std::random_device{}())] {
          return (*engine)();
        });
    command_wake_ = std::make_unique<EventFd>();

    loop_.watch_socket(*net_, net_->fd());
    loop_.watch_stop_wake(*command_wake_);

    metrics_ = std::make_unique<MetricsWorker>(snapshot_queue_, metrics_path_);
    metrics_->start();

    node_->start_join(real_clock_->now());
    running_ = true;
  } catch (...) {
    rollback();
    throw;
  }
}

MetricsSnapshot Client::run(MonoNs duration) {
  if (!running_) {
    throw std::runtime_error("Client::run before start");
  }
  const MonoNs end_at = duration > 0 ? real_clock_->now() + duration : 0;
  next_metrics_ = real_clock_->now() + cfg_.metrics_interval;
  loop_.run(*this, *real_clock_, end_at);

  node_->shutdown(real_clock_->now());
  MetricsSnapshot final_snapshot = build_snapshot(true);
  snapshot_queue_.try_push(final_snapshot);  // copies; final never dropped
  running_ = false;
  return final_snapshot;
}

void Client::stop() {
  if (command_wake_) command_wake_->notify();
}

// --- EventLoop::Delegate: the client's policy over the shared machinery ---

void Client::drain_input(MonoNs now) {
  // Readable UDP drained to EAGAIN (level-triggered, §3.2).
  loop_.drain_socket(
      [this, now](Datagram&& dg) { node_->on_datagram(std::move(dg), now); },
      [this] { node_->counters_.malformed++; });
  reorder_hwm_ =
      std::max<std::uint64_t>(reorder_hwm_, node_->reorder_depth());
  jitter_hwm_ =
      std::max<std::uint64_t>(jitter_hwm_, node_->jitter_depth());
  mem_hwm_ = std::max<std::uint64_t>(mem_hwm_, node_->bytes_owned());
}

void Client::on_timer(const TimerEntry& e, MonoNs now) {
  if (e.kind == TimerKind::SESSION_SCAN) {
    if (!scan_deadline_ || e.deadline != *scan_deadline_) return;
    scan_deadline_.reset();
    node_->on_timer(now);
  }
}

bool Client::pump(INetwork& net, MonoNs now) { return node_->pump(net, now); }

void Client::after_wake(MonoNs now) {
  reorder_hwm_ =
      std::max<std::uint64_t>(reorder_hwm_, node_->reorder_depth());
  jitter_hwm_ =
      std::max<std::uint64_t>(jitter_hwm_, node_->jitter_depth());
  mem_hwm_ = std::max<std::uint64_t>(mem_hwm_, node_->bytes_owned());
  (void)node_->take_deliveries();  // drain consumer queue so real process doesn't drop

  if (now >= next_metrics_) {
    snapshot_queue_.try_push(build_snapshot(false));
    next_metrics_ = now + cfg_.metrics_interval;
  }

  // Keep a single earliest deadline armed in the timer heap (matching
  // Gateway::after_wake); replacing in-place avoids stale duplicates when an
  // incoming packet pulls the deadline earlier.
  const auto deadline = node_->next_deadline();
  if (!deadline) {
    if (scan_deadline_) {
      loop_.erase_session_timers(0);
      scan_deadline_.reset();
    }
    return;
  }
  if (!scan_deadline_ || *deadline < *scan_deadline_) {
    TimerEntry e;
    e.kind = TimerKind::SESSION_SCAN;
    e.deadline = *deadline;
    if (loop_.replace_session_timer(e)) {
      scan_deadline_ = *deadline;
    }
  }
}

bool Client::finished() const { return true; }  // no drain barrier

bool Client::metrics_final_acked() const {
  return metrics_ ? metrics_->final_acked() : false;
}

MetricsSnapshot Client::build_snapshot(bool final_snapshot) {
  MetricsSnapshot s;
  s.taken_at = real_clock_->now();
  s.is_final = final_snapshot;
  s.clock_mode = "real";
  s.packets = node_->counters_;
  s.packets += node_->receiver_counters();
  s.delivered_ontime = node_->delivered_ontime();
  s.delivered_late = node_->delivered_late();
  s.delivery_expired = node_->delivery_expired();
  s.delivery_abandoned = node_->delivery_abandoned();
  s.playout_dropped = node_->playout_dropped();
  s.consumer_dropped = node_->consumer_dropped();
  s.queue_depth_hwm_control = static_cast<std::uint32_t>(node_->control_hwm());
  s.queue_depth_hwm_reorder = static_cast<std::uint32_t>(
      std::max<std::uint64_t>(reorder_hwm_, node_->reorder_depth()));
  s.queue_depth_hwm_jitter = static_cast<std::uint32_t>(
      std::max<std::uint64_t>(jitter_hwm_, node_->jitter_depth()));
  s.queue_depth_hwm_timer =
      static_cast<std::uint32_t>(loop_.timers().high_water());
  s.memory_high_water_bytes =
      std::max<std::uint64_t>(mem_hwm_, node_->bytes_owned());
  return s;
}

}  // namespace rudp_rt
