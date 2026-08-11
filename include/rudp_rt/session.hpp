#pragma once
// rudp_rt/session.hpp — session lifecycle and the socket-free protocol cores.
//
// GatewayNode and ClientNode implement everything above the datagram
// boundary: handshake/epochs, fan-out admission, the sender/receiver state
// machines, per-session queues, token buckets, keepalive/timeout, and ledger
// accounting. The real Reactor (runtime.hpp) and the simulator both drive
// them through the same event API:
//
//   on_datagram(dg, now) -> protocol input
//   on_timer/on_session_timer(id, gen, now) -> due decisions
//   offer_frame / capture feed -> fan-out admission
//   pump(net, now) -> bounded, prioritized emission via INetwork
//
// INV-10: protocol state is mutated only from the owning thread.

#include "rudp_rt/clock.hpp"
#include "rudp_rt/config.hpp"
#include "rudp_rt/metrics.hpp"
#include "rudp_rt/network.hpp"
#include "rudp_rt/playout.hpp"
#include "rudp_rt/receiver.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/types.hpp"
#include "rudp_rt/wire.hpp"

#include <deque>
#include <map>
#include <optional>
#include <vector>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Control queue: bounded, ABANDON-preserving (ADR-0007).
// ---------------------------------------------------------------------------
class ControlQueue {
 public:
  ControlQueue() = default;  // for move-assign into session members
  explicit ControlQueue(int capacity) : capacity_(capacity) {}

  const Datagram& front() const { return items_.front(); }
  // Returns false when the datagram was dropped (counted by the caller).
  bool push(Datagram&& dg) {
    if (static_cast<int>(items_.size()) < capacity_) {
      items_.push_back(std::move(dg));
      return true;
    }
    const bool incoming_is_abandon =
        !dg.bytes.empty() &&
        static_cast<std::uint8_t>(dg.bytes[1]) ==
            static_cast<std::uint8_t>(PacketType::ABANDON);
    // Preserve ABANDON over non-critical control.
    for (std::size_t i = 0; i < items_.size(); ++i) {
      const bool is_abandon =
          !items_[i].bytes.empty() &&
          static_cast<std::uint8_t>(items_[i].bytes[1]) ==
              static_cast<std::uint8_t>(PacketType::ABANDON);
      if (incoming_is_abandon && !is_abandon) {
        items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(i));
        items_.push_back(std::move(dg));
        return true;  // caller counts the dropped non-critical item
      }
    }
    return false;  // bounded cap wins even for ABANDON
  }

  bool empty() const { return items_.empty(); }
  std::size_t size() const { return items_.size(); }
  int capacity() const { return capacity_; }
  Datagram pop() {
    Datagram dg = std::move(items_.front());
    items_.pop_front();
    return dg;
  }
  std::size_t high_water() const { return hwm_; }
  void observe_hwm() {
    if (items_.size() > hwm_) hwm_ = items_.size();
  }

 private:
  int capacity_ = 0;
  std::deque<Datagram> items_;
  std::size_t hwm_ = 0;
};

// ---------------------------------------------------------------------------
// Token bucket (ADR-0008). Refill is computed lazily against the clock.
// ---------------------------------------------------------------------------
class TokenBucket {
 public:
  void configure(std::int64_t rate_bytes_per_sec, std::int64_t burst_bytes) {
    rate_ = rate_bytes_per_sec;
    burst_ = burst_bytes;
    tokens_ = burst_bytes;
  }
  void refill(MonoNs now) {
    if (last_refill_ < 0) last_refill_ = now;
    const MonoNs delta = now - last_refill_;
    if (delta <= 0) return;
    last_refill_ = now;
    // rate (<= 2^31) * delta (capped) stays far inside int64.
    const MonoNs capped = std::min<MonoNs>(delta, kNsPerSec * 3600);
    const std::int64_t gained =
        static_cast<std::int64_t>((static_cast<std::int64_t>(rate_) * capped) /
                                  kNsPerSec);
    tokens_ = std::min<std::int64_t>(burst_, tokens_ + gained);
  }
  bool try_take(std::int64_t bytes) {
    if (tokens_ >= bytes) {
      tokens_ -= bytes;
      return true;
    }
    return false;
  }
  std::int64_t tokens() const { return tokens_; }

 private:
  std::int64_t rate_ = 1;
  std::int64_t burst_ = 1;
  std::int64_t tokens_ = 1;
  MonoNs last_refill_ = -1;
};

// ---------------------------------------------------------------------------
// Server-side session
// ---------------------------------------------------------------------------
class Session {
 public:
  Session(std::uint32_t id, std::uint32_t generation, const Endpoint& peer,
          const Config& cfg, std::uint32_t epoch, std::uint32_t isn,
          IClock& clock);

  std::uint32_t id = 0;
  std::uint32_t generation = 0;
  Endpoint peer;
  std::uint16_t stream_id = 0;
  std::uint32_t session_epoch = 0;
  std::uint32_t server_isn = 0;
  std::uint32_t client_epoch = 0;   // for idempotent JOIN detection (ADR-0003)
  MonoNs last_join_replace = 0;     // JOIN replacement throttle (ADR-0013)
  bool dead = false;
  TeardownCause teardown_cause = TeardownCause::NONE;

  TransportSender sender;
  ControlQueue control;
  TokenBucket data_tokens;
  TokenBucket control_tokens;
  std::optional<Datagram> pending_out;  // EAGAIN slot (ADR-0009)

  SessionFrameLedger frames;
  PacketCounters packets;

  // Emission helpers used by GatewayNode::pump.
  std::deque<Datagram> data_queue;  // retransmitted DATA awaiting tokens
  std::size_t data_queue_hwm = 0;

  // --- liveness policy (spec §4.3); the formulas live here, once ---
  // Called at session creation: every liveness clock starts at `now`.
  void note_established(MonoNs now);
  void note_outbound(MonoNs now);
  void note_ack(MonoNs now);  // valid cumulative ACK: starvation evidence
  void arm_probe(std::uint32_t nonce, MonoNs now);
  bool probe_matches(std::uint32_t nonce) const;
  void probe_answered();
  std::uint32_t unanswered_probes() const { return unanswered_probes_; }

  // One liveness scan step: watchdog rising edge, ACK starvation, probe
  // cadence. The caller executes the decisions (violation counter, PING
  // build, teardown) — the policy and its formulas stay here.
  struct LivenessScan {
    bool watchdog_fired = false;            // rising edge: stall began
    bool probe_due = false;                 // send a keepalive probe now
    std::optional<TeardownCause> teardown;  // SESSION_TIMEOUT when terminal
  };
  LivenessScan liveness_scan(const Config& cfg, MonoNs now);

  // Earliest deadline among sender timers, keepalive scheduling, and the
  // ACK-starvation watch — the same formulas liveness_scan enforces.
  MonoNs next_timer_deadline(const Config& cfg) const;

 private:
  MonoNs last_outbound_ = 0;
  MonoNs last_valid_ack_time_ = 0;
  MonoNs last_probe_sent_ = 0;
  std::uint32_t last_probe_nonce_ = 0;
  std::uint32_t unanswered_probes_ = 0;
  bool watchdog_armed_ = false;
};

// ---------------------------------------------------------------------------
// GatewayNode — server protocol core (socket-free).
// ---------------------------------------------------------------------------
class GatewayNode {
 public:
  // rng supplies fresh random epochs/ISNs/nonces; production seeds it from
  // std::random_device, the simulator from the scenario seed (reproducibility,
  // spec §0.1 #4).
  GatewayNode(const Config& cfg, IClock& clock,
              std::function<std::uint32_t()> rng);

  // --- input path (owning thread only) ---
  void on_datagram(const Datagram& dg, MonoNs now);
  // Fan-out admission for one captured frame (spec §6: immutable session set).
  void offer_frame(MediaFrame&& frame, MonoNs now);

  // --- timer path ---
  std::optional<MonoNs> next_deadline() const;
  MonoNs session_next_deadline(std::uint32_t session_id) const;
  void on_session_timer(std::uint32_t session_id, std::uint32_t generation,
                        MonoNs now);
  void on_timer(MonoNs now);  // scans every live session (sim/one-shot path)

  // --- emission ---
  // Returns true when output remains pending (EPOLLOUT should stay armed).
  bool pump(INetwork& net, MonoNs now);

  // --- lifecycle ---
  // Marks the shutdown barrier (INV-18): no new admissions, in-flight frames
  // finalized ABANDONED(SHUTDOWN), final ABANDONs queued.
  void begin_shutdown(MonoNs now);
  void teardown_session(std::uint32_t session_id, TeardownCause cause,
                        MonoNs now);
  bool shutting_down() const { return shutting_down_; }
  // True when every queue is empty and no output is pending: the drain
  // condition for bounded graceful shutdown (INV-17/INV-18).
  bool output_drained() const;

  // --- observability ---
  std::size_t session_count() const { return sessions_.size(); }
  std::vector<std::pair<std::uint32_t, std::uint32_t>> live_sessions()
      const;  // (id, generation) of live sessions, key order
  std::vector<SessionMetrics> collect_sessions() const;
  PacketCounters& counters() { return counters_; }
  const PacketCounters& counters() const { return counters_; }
  std::uint32_t watchdog_violations() const { return watchdog_violations_; }
  std::uint32_t resource_exhausted() const { return resource_exhausted_; }
  std::uint64_t memory_high_water() const { return memory_hwm_; }
  std::size_t control_hwm() const;
  SessionFrameLedger aggregate_ledger() const;
  void set_keyframe_request_callback(std::function<void()> cb) {
    on_keyframe_request_ = std::move(cb);
  }

  static constexpr std::uint32_t kNoSession = 0xFFFFFFFFu;

 private:
  struct JoinDecision {
    PongStatus status = PongStatus::OK;
    Session* session = nullptr;  // existing session for idempotent replies
  };

  Session* find_session(const Endpoint& peer, std::uint16_t stream_id);
  void handle_ping(const Datagram& dg, const Header& h, MonoNs now);
  void handle_pong(const Datagram& dg, const Header& h, MonoNs now);
  void handle_ack_or_nack(const Datagram& dg, const Header& h, bool is_ack,
                          Session& s, MonoNs now);
  void handle_join(Session* existing, const Endpoint& peer,
                   const Header& h, const Ping& ping, MonoNs now);
  Session* create_session(const Endpoint& peer, std::uint16_t stream_id,
                          std::uint32_t client_epoch, MonoNs now);
  void send_pong(Session& s, std::uint32_t nonce, PongStatus status,
                 std::uint32_t epoch, std::uint32_t isn);
  void send_pong_node(const Endpoint& peer, std::uint32_t nonce,
                      PongStatus status, std::uint32_t epoch,
                      std::uint32_t isn);
  void queue_ping_keepalive(Session& s, MonoNs now);
  void route_sender_output(Session& s);
  void pump_session(Session& s, INetwork& net, MonoNs now,
                    const std::function<int(Datagram&, Session*)>& send_one);
  void check_session_liveness(Session& s, MonoNs now);
  void scan_session(Session& s, MonoNs now);
  bool control_rate_admit(const Endpoint& peer, MonoNs now);

  const Config& cfg_;
  IClock& clock_;
  // Session key: (endpoint, stream_id). v1 configures one stream per process,
  // but the key is stream-qualified so the code stays honest (spec §4.3).
  using SessionKey = std::pair<Endpoint, std::uint16_t>;
  std::map<SessionKey, Session> sessions_;
  std::uint32_t next_session_id_ = 1;
  std::uint32_t next_nonce_ = 1;
  bool shutting_down_ = false;

  ControlQueue node_control_{64};  // PONG replies for unknown/unsynced peers
  TokenBucket node_control_tokens_;
  std::optional<Datagram> node_pending_;

  std::function<std::uint32_t()> rng_;

  // Per-endpoint control rate limiter (ADR-0013), bounded map.
  struct RateState {
    TokenBucket bucket;
    MonoNs last_seen = 0;
  };
  std::map<Endpoint, RateState> rate_limits_;

  PacketCounters counters_;
  std::uint32_t watchdog_violations_ = 0;
  std::uint32_t resource_exhausted_ = 0;
  std::uint64_t memory_hwm_ = 0;
  std::size_t control_hwm_ = 0;
  std::size_t pump_cursor_ = 0;  // round-robin rotation
  std::uint32_t teardown_counter_ = 0;
  std::function<void()> on_keyframe_request_;
};

// ---------------------------------------------------------------------------
// ClientNode — reference client protocol core (socket-free, ADR-0015).
// ---------------------------------------------------------------------------
class ClientNode {
 public:
  ClientNode(const Config& cfg, IClock& clock, Endpoint gateway,
             std::uint16_t stream_id, std::function<std::uint32_t()> rng);

  // --- lifecycle ---
  void start_join(MonoNs now);
  bool established() const { return state_ == State::ESTABLISHED; }
  bool joining() const { return state_ == State::JOINING; }
  void shutdown(MonoNs now);  // stops timers; delivery pipeline intact

  // --- input path ---
  void on_datagram(const Datagram& dg, MonoNs now);

  // --- timer path ---
  void on_timer(MonoNs now);
  std::optional<MonoNs> next_deadline() const;

  // --- emission ---
  // Returns true when output remains pending (EPOLLOUT stays armed).
  bool pump(INetwork& net, MonoNs now);

  // --- delivery output ---
  std::vector<JitterBuffer::Delivery> take_deliveries();

  // --- observability ---
  PacketCounters counters_;
  // Playout accounting (INV-21) and the delivery consumer queue are owned by
  // the PlayoutPipeline; the node exposes read-only accessors.
  std::uint64_t delivered_ontime() const { return playout_.delivered_ontime(); }
  std::uint64_t delivered_late() const { return playout_.delivered_late(); }
  std::uint64_t delivery_expired() const { return playout_.delivery_expired(); }
  std::uint64_t delivery_abandoned() const {
    return playout_.delivery_abandoned();
  }
  std::uint64_t playout_dropped() const { return playout_.playout_dropped(); }
  std::uint64_t consumer_dropped() const { return playout_.consumer_dropped(); }
  std::uint64_t rejects_received = 0;
  PongStatus last_reject = PongStatus::OK;
  std::uint64_t bytes_owned() const;

  // Benchmark observation (ADR-0012): per-frame delivery latency from the
  // RTP-derived capture estimate, plus the delivered frame ids.
  void record_latencies(bool enable) { playout_.record_latencies(enable); }
  const std::vector<MonoNs>& latencies() const { return playout_.latencies(); }
  const std::vector<std::uint32_t>& delivered_frame_ids() const {
    return playout_.delivered_frame_ids();
  }
  std::size_t jitter_depth() const { return playout_.jitter_depth(); }
  std::size_t reorder_depth() const { return playout_.reorder_depth(); }
  std::size_t control_depth() const { return control_.size(); }
  std::size_t control_hwm() const { return control_.high_water(); }
  MonoNs srtt() const { return rtt_.srtt(); }
  // Component-level counters (the node's own counters only count node-level
  // events; the receiver keeps its own inside the pipeline).
  PacketCounters receiver_counters() const {
    return playout_.receiver_counters();
  }

 private:
  enum class State { JOINING, ESTABLISHED, DOWN };

  void handle_data(const Datagram& dg, const Header& h, MonoNs now);
  void handle_ping(const Datagram& dg, const Header& h, MonoNs now);
  void handle_pong(const Datagram& dg, const Header& h, MonoNs now);
  void handle_abandon(const Datagram& dg, const Header& h, MonoNs now);
  void send_join(MonoNs now);
  void queue_pong(std::uint32_t nonce, PongStatus status);
  void route_control(std::vector<Datagram> dgs);
  void check_keepalive(MonoNs now);
  void note_outbound(MonoNs now);

  const Config& cfg_;
  IClock& clock_;
  Endpoint gateway_;
  std::uint16_t stream_id_ = 0;
  std::uint32_t client_epoch_ = 0;
  std::uint32_t session_epoch_ = 0;
  std::uint32_t server_isn_ = 0;
  State state_ = State::JOINING;

  std::function<std::uint32_t()> rng_;
  PlayoutPipeline playout_;
  RtoEstimator rtt_;

  ControlQueue control_;
  TokenBucket tokens_;
  std::optional<Datagram> pending_out_;

  std::uint32_t join_nonce_ = 0;
  MonoNs last_join_send_ = 0;
  std::uint32_t probe_nonce_ = 0;
  MonoNs last_probe_send_ = 0;
  std::uint32_t unanswered_probes_ = 0;
  MonoNs last_outbound_ = 0;
  MonoNs last_inbound_ = 0;
  std::uint32_t nonce_counter_ = 1;
  bool torn_down_ = false;
};

}  // namespace rudp_rt
