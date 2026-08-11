// session_manager.cpp — GatewayNode: server-side session lifecycle,
// handshake/epochs, fan-out admission, scheduling budgets (spec §3, §4.2–§4.3).

#include "rudp_rt/session.hpp"

#include <algorithm>
#include <memory>
#include <random>

namespace rudp_rt {

namespace {

std::function<std::uint32_t()> default_rng() {
  auto engine = std::make_shared<std::mt19937>(std::random_device{}());
  return [engine]() { return (*engine)(); };
}

std::span<const std::byte> payload_of(const Datagram& dg) {
  return std::span<const std::byte>(dg.bytes).subspan(kHeaderSize);
}

Header control_header(std::uint16_t stream_id, PacketType type,
                      std::uint16_t payload_len) {
  Header h;
  h.version = kWireVersion;
  h.header_len = kHeaderLenWords;
  h.type = static_cast<std::uint8_t>(type);
  h.stream_id = stream_id;
  h.payload_len = payload_len;
  return h;
}

Datagram build_pong(const Endpoint& peer, std::uint16_t stream_id,
                    const Pong& pong) {
  Datagram dg;
  dg.peer = peer;
  dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
  encode_header(dg.bytes.data(),
                control_header(stream_id, PacketType::PONG,
                               static_cast<std::uint16_t>(encoded_pong_size())));
  encode_pong(dg.bytes.data() + kHeaderSize, pong);
  return dg;
}

Datagram build_ping(const Endpoint& peer, std::uint16_t stream_id,
                    const Ping& ping) {
  Datagram dg;
  dg.peer = peer;
  const std::size_t len = encoded_ping_size(ping.purpose);
  dg.bytes.assign(kHeaderSize + len, std::byte{0});
  encode_header(dg.bytes.data(),
                control_header(stream_id, PacketType::PING,
                               static_cast<std::uint16_t>(len)));
  encode_ping(dg.bytes.data() + kHeaderSize, ping);
  return dg;
}

}  // namespace

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

Session::Session(std::uint32_t id_in, std::uint32_t generation_in,
                 const Endpoint& peer_in, const Config& cfg,
                 std::uint32_t epoch, std::uint32_t isn, IClock& clock)
    : id(id_in),
      generation(generation_in),
      peer(peer_in),
      stream_id(cfg.stream_id),
      session_epoch(epoch),
      server_isn(isn) {
  SenderConfig sc;
  sc.peer = peer;
  sc.stream_id = stream_id;
  sc.ssrc = cfg.ssrc ? cfg.ssrc : (0x5EED0000u ^ stream_id);
  sc.session_epoch = epoch;
  sc.isn = isn;
  sc.window_w = cfg.window_w;
  sc.max_frame_packets = cfg.max_frame_packets;
  sc.media_payload_bytes = cfg.media_payload_bytes;
  sc.rto_min = cfg.rto_min;
  sc.rto_max = cfg.rto_max;
  sc.srtt_initial = cfg.srtt_initial;
  sc.rttvar_initial = cfg.rttvar_initial;
  sc.deadline_mode = cfg.deadline_mode;
  sender = TransportSender(sc, clock);
  control = ControlQueue(cfg.control_queue_datagrams);
  data_tokens.configure(cfg.token_rate_bytes_per_sec, cfg.token_burst_bytes);
  control_tokens.configure(cfg.token_rate_bytes_per_sec,
                           cfg.control_reserve_bytes);
}

std::optional<MonoNs> GatewayNode::next_deadline() const {
  std::optional<MonoNs> best;
  for (const auto& [key, s] : sessions_) {
    (void)key;
    if (s.dead) continue;
    const MonoNs d = s.next_timer_deadline(cfg_);
    if (!best || d < *best) best = d;
  }
  return best;
}

MonoNs GatewayNode::session_next_deadline(std::uint32_t session_id) const {
  for (const auto& [key, s] : sessions_) {
    (void)key;
    if (s.id == session_id) return s.next_timer_deadline(cfg_);
  }
  return clock_.now();
}

MonoNs Session::next_timer_deadline(const Config& cfg) const {
  std::optional<MonoNs> best = sender.next_deadline();
  const auto consider = [&best](MonoNs t) {
    if (!best || t < *best) best = t;
  };
  // Keepalive probe scheduling (spec §4.3).
  consider(std::max(last_outbound_ + cfg.keepalive_silence_after,
                    last_probe_sent_ + cfg.keepalive_min_interval));
  // ACK starvation watch (spec §4.3) while the window is non-empty.
  if (!sender.window_empty()) {
    consider(last_valid_ack_time_ +
             static_cast<MonoNs>(cfg.ack_starvation_rto_intervals) *
                 cfg.rto_max);
  }
  return *best;
}

void Session::note_established(MonoNs now) {
  last_outbound_ = now;
  last_valid_ack_time_ = now;
}

void Session::note_outbound(MonoNs now) { last_outbound_ = now; }

void Session::note_ack(MonoNs now) { last_valid_ack_time_ = now; }

void Session::arm_probe(std::uint32_t nonce, MonoNs now) {
  last_probe_nonce_ = nonce;
  last_probe_sent_ = now;
  unanswered_probes_++;
}

bool Session::probe_matches(std::uint32_t nonce) const {
  return last_probe_nonce_ != 0 && nonce == last_probe_nonce_;
}

void Session::probe_answered() { unanswered_probes_ = 0; }

Session::LivenessScan Session::liveness_scan(const Config& cfg, MonoNs now) {
  LivenessScan scan;
  if (dead) return scan;

  // Watchdog: window stalled for 2xRTO_MAX without ACK progress (v0.2 §3.14).
  if (!sender.window_empty() &&
      now - last_valid_ack_time_ >= 2 * cfg.rto_max) {
    if (!watchdog_armed_) {
      watchdog_armed_ = true;
      scan.watchdog_fired = true;
    }
  } else {
    watchdog_armed_ = false;
  }

  // ACK starvation: three RTO_MAX intervals without a valid ACK terminates
  // the session (spec §4.3); no probe is queued on this path.
  if (!sender.window_empty() &&
      now - last_valid_ack_time_ >=
          static_cast<MonoNs>(cfg.ack_starvation_rto_intervals) *
              cfg.rto_max) {
    scan.teardown = TeardownCause::SESSION_TIMEOUT;
    return scan;
  }

  // Keepalive probes after outbound silence (spec §4.3).
  const MonoNs silence = now - last_outbound_;
  if (silence >= cfg.keepalive_silence_after &&
      now - last_probe_sent_ >= cfg.keepalive_min_interval) {
    scan.probe_due = true;
  }
  if (unanswered_probes_ >=
      static_cast<std::uint32_t>(cfg.keepalive_max_probes)) {
    scan.teardown = TeardownCause::SESSION_TIMEOUT;
  }
  return scan;
}

// ---------------------------------------------------------------------------
// GatewayNode
// ---------------------------------------------------------------------------

GatewayNode::GatewayNode(const Config& cfg, IClock& clock,
                         std::function<std::uint32_t()> rng)
    : cfg_(cfg), clock_(clock), rng_(rng ? std::move(rng) : default_rng()) {
  node_control_tokens_.configure(cfg.token_rate_bytes_per_sec,
                                 cfg.control_reserve_bytes);
}

Session* GatewayNode::find_session(const Endpoint& peer,
                                   std::uint16_t stream_id) {
  auto it = sessions_.find({peer, stream_id});
  return it == sessions_.end() ? nullptr : &it->second;
}

void GatewayNode::on_datagram(const Datagram& dg, MonoNs now) {
  auto decoded = decode_header(dg.bytes);
  if (decoded.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  const Header& h = decoded.header;
  const auto type = static_cast<PacketType>(h.type);

  switch (type) {
    case PacketType::PING:
      // PING (Join/Restart/Keepalive) is the flood-sensitive family
      // (ADR-0013); ACK/NACK are session-scoped and bounded by the data
      // path plus queue caps, so they bypass the endpoint limiter.
      if (!control_rate_admit(dg.peer, now)) return;
      handle_ping(dg, h, now);
      return;
    case PacketType::PONG:
      handle_pong(dg, h, now);
      return;
    case PacketType::ACK:
    case PacketType::NACK:
      break;  // handled below with the session lookup
    case PacketType::DATA:
    case PacketType::ABANDON:
      // Wrong direction on the server (clients never send these).
      counters_.protocol_violations++;
      return;
  }

  Session* s = find_session(dg.peer, h.stream_id);
  if (s == nullptr || s->dead) {
    counters_.unknown_session_datagrams++;
    return;
  }
  handle_ack_or_nack(dg, h, type == PacketType::ACK, *s, now);
}

void GatewayNode::handle_ping(const Datagram& dg, const Header& h,
                              MonoNs now) {
  auto ping = decode_ping(payload_of(dg));
  if (ping.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  counters_.pings_received++;

  if (h.stream_id != cfg_.stream_id) {
    send_pong_node(dg.peer, ping.ping.nonce, PongStatus::REJECT_UNKNOWN_STREAM,
                   0, 0);
    return;
  }

  Session* existing = find_session(dg.peer, h.stream_id);
  if (ping.ping.purpose == PingPurpose::KEEPALIVE) {
    if (existing == nullptr || existing->dead) {
      send_pong_node(dg.peer, ping.ping.nonce, PongStatus::REJECT_STALE, 0, 0);
      return;
    }
    if (ping.ping.session_epoch != existing->session_epoch) {
      // Stale-epoch KEEPALIVE: resync help, no state mutation (ADR-0003).
      send_pong_node(dg.peer, ping.ping.nonce, PongStatus::REJECT_STALE,
                     existing->session_epoch, existing->server_isn);
      counters_.stale_control++;
      return;
    }
    send_pong(*existing, ping.ping.nonce, PongStatus::OK,
              existing->session_epoch, existing->server_isn);
    return;
  }

  // JOIN / RESTART (RESTART is equivalent to JOIN with a new client epoch).
  handle_join(existing, dg.peer, h, ping.ping, now);
}

void GatewayNode::handle_join(Session* existing, const Endpoint& peer,
                              const Header& h, const Ping& ping, MonoNs now) {
  if (existing != nullptr && !existing->dead &&
      ping.client_epoch == existing->client_epoch) {
    // Idempotent duplicate JOIN: same PONG values (spec §4.3).
    send_pong(*existing, ping.nonce, PongStatus::OK, existing->session_epoch,
              existing->server_isn);
    return;
  }
  if (existing != nullptr && !existing->dead &&
      now - existing->last_join_replace < kNsPerMs * 100) {
    // Replacement throttle (ADR-0013).
    send_pong_node(peer, ping.nonce, PongStatus::REJECT_STALE,
                   existing->session_epoch, existing->server_isn);
    counters_.stale_control++;
    return;
  }
  if (static_cast<int>(sessions_.size()) >= cfg_.max_sessions &&
      (existing == nullptr || existing->dead)) {
    send_pong_node(peer, ping.nonce, PongStatus::REJECT_MAX_SESSIONS, 0, 0);
    return;
  }

  // Replace: fully mark the old session dead first (spec §4.3), then create.
  if (existing != nullptr && !existing->dead) {
    teardown_session(existing->id, TeardownCause::REPLACED, now);
  }
  Session* s = create_session(peer, h.stream_id, ping.client_epoch, now);
  send_pong(*s, ping.nonce, PongStatus::OK, s->session_epoch, s->server_isn);
}

Session* GatewayNode::create_session(const Endpoint& peer,
                                     std::uint16_t stream_id,
                                     std::uint32_t client_epoch, MonoNs now) {
  const std::uint32_t id = next_session_id_++;
  const std::uint32_t epoch = rng_();
  const std::uint32_t isn = rng_();
  const SessionKey key{peer, stream_id};
  // A dead session at this key must be replaced, not reused (map::emplace
  // would return the stale entry and the client could never rejoin).
  sessions_.erase(key);
  auto [it, inserted] = sessions_.emplace(
      key, Session(id, id, peer, cfg_, epoch, isn, clock_));
  (void)inserted;
  Session& s = it->second;
  s.client_epoch = client_epoch;
  s.last_join_replace = now;
  s.note_established(now);
  return &s;
}

void GatewayNode::send_pong(Session& s, std::uint32_t nonce, PongStatus status,
                            std::uint32_t epoch, std::uint32_t isn) {
  Pong pong;
  pong.nonce = nonce;
  pong.status = status;
  pong.session_epoch = epoch;
  pong.server_isn = isn;
  Datagram dg = build_pong(s.peer, s.stream_id, pong);
  if (!s.control.push(std::move(dg))) {
    s.packets.control_queue_dropped++;
  } else {
    s.control.observe_hwm();
  }
  counters_.pongs_sent++;
}

void GatewayNode::send_pong_node(const Endpoint& peer, std::uint32_t nonce,
                                 PongStatus status, std::uint32_t epoch,
                                 std::uint32_t isn) {
  Pong pong;
  pong.nonce = nonce;
  pong.status = status;
  pong.session_epoch = epoch;
  pong.server_isn = isn;
  Datagram dg = build_pong(peer, cfg_.stream_id, pong);
  if (!node_control_.push(std::move(dg))) {
    counters_.control_queue_dropped++;
  }
  counters_.pongs_sent++;
}

void GatewayNode::handle_pong(const Datagram& dg, const Header& h,
                              MonoNs now) {
  (void)now;  // probe bookkeeping lives in Session
  auto pong = decode_pong(payload_of(dg));
  if (pong.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  Session* s = find_session(dg.peer, h.stream_id);
  if (s == nullptr || s->dead) {
    counters_.unknown_session_datagrams++;
    return;
  }
  if (pong.pong.session_epoch != s->session_epoch) {
    counters_.stale_control++;
    return;
  }
  if (!s->probe_matches(pong.pong.nonce)) {
    counters_.stale_control++;  // unknown nonce (late/foreign reply)
    return;
  }
  s->probe_answered();  // probe answered
}

void GatewayNode::handle_ack_or_nack(const Datagram& dg, const Header&,
                                     bool is_ack, Session& s, MonoNs now) {
  if (is_ack) {
    auto ack = decode_ack(payload_of(dg));
    if (ack.reject != RejectReason::NONE) {
      counters_.malformed++;
      return;
    }
    s.sender.on_ack(ack.ack, now);
    s.note_ack(now);  // evidence of liveness (spec §4.3)
  } else {
    auto nack = decode_nack(payload_of(dg));
    if (nack.reject != RejectReason::NONE) {
      counters_.malformed++;
      return;
    }
    s.sender.on_nack(nack.nack, now);
  }
  route_sender_output(s);
  const std::uint64_t mem = s.sender.media_bytes_owned();
  memory_hwm_ = std::max(memory_hwm_, mem);
}

void GatewayNode::route_sender_output(Session& s) {
  SenderOutput out = s.sender.take_output();
  for (const auto& adm : out.admissions) {
    if (adm.admitted) {
      s.frames.admit();
    } else {
      s.frames.close(SessionFrameOutcome::PRE_ADMISSION_DROPPED);
    }
  }
  for (const auto& c : out.closures) {
    s.frames.close(c.outcome);
  }
  for (auto& dg : out.control) {
    if (!s.control.push(std::move(dg))) {
      s.packets.control_queue_dropped++;
    } else {
      s.control.observe_hwm();
      control_hwm_ = std::max(control_hwm_, s.control.high_water());
    }
  }
  for (auto& dg : out.data) {
    s.data_queue.push_back(std::move(dg));
    s.data_queue_hwm = std::max(s.data_queue_hwm, s.data_queue.size());
  }
  if (out.keyframe_requested && on_keyframe_request_) {
    on_keyframe_request_();
  }
}

void GatewayNode::offer_frame(MediaFrame&& frame, MonoNs now) {
  (void)now;  // admission uses the injected clock internally (spec §5)
  if (shutting_down_) return;  // shutdown barrier (INV-18)
  if (sessions_.empty()) return;  // zero-session fan-out: no ledger entries
  // Deterministic iteration order: std::map key order (endpoint, stream).
  for (auto& [key, s] : sessions_) {
    (void)key;
    if (s.dead) continue;
    MediaFrame copy = frame;  // v1: no cross-session payload sharing (D-31)
    s.sender.on_frame(std::move(copy));
    route_sender_output(s);
  }
  std::uint64_t mem = 0;
  for (auto& [key, s] : sessions_) {
    (void)key;
    mem += s.sender.media_bytes_owned();
  }
  memory_hwm_ = std::max(memory_hwm_, mem);
}

void GatewayNode::on_session_timer(std::uint32_t session_id,
                                   std::uint32_t generation, MonoNs now) {
  for (auto& [key, s] : sessions_) {
    (void)key;
    if (s.id == session_id) {
      if (s.dead || s.generation != generation) return;  // stale (INV-14)
      scan_session(s, now);
      return;
    }
  }
}

void GatewayNode::on_timer(MonoNs now) {
  for (auto& [key, s] : sessions_) {
    (void)key;
    if (!s.dead) scan_session(s, now);
  }
}

void GatewayNode::scan_session(Session& s, MonoNs now) {
  s.sender.on_timer(now);
  route_sender_output(s);
  check_session_liveness(s, now);
}

void GatewayNode::check_session_liveness(Session& s, MonoNs now) {
  // Policy and formulas live in Session; the node only executes decisions.
  const Session::LivenessScan scan = s.liveness_scan(cfg_, now);
  if (scan.watchdog_fired) {
    watchdog_violations_++;
  }
  if (scan.probe_due) {
    queue_ping_keepalive(s, now);
  }
  if (scan.teardown) {
    teardown_session(s.id, *scan.teardown, now);
  }
}

void GatewayNode::queue_ping_keepalive(Session& s, MonoNs now) {
  Ping ping;
  ping.nonce = ++next_nonce_;
  ping.purpose = PingPurpose::KEEPALIVE;
  ping.session_epoch = s.session_epoch;
  ping.client_epoch = s.client_epoch;
  s.arm_probe(ping.nonce, now);
  Datagram dg = build_ping(s.peer, s.stream_id, ping);
  if (!s.control.push(std::move(dg))) {
    s.packets.control_queue_dropped++;
  }
  counters_.pings_sent++;
}

void GatewayNode::teardown_session(std::uint32_t session_id,
                                   TeardownCause cause, MonoNs now) {
  for (auto& [key, s] : sessions_) {
    (void)key;
    if (s.id != session_id || s.dead) continue;
    s.dead = true;
    s.teardown_cause = cause;
    s.generation++;  // invalidates outstanding timer entries (INV-14)
    s.sender.teardown(cause, now);
    route_sender_output(s);
    if (cause != TeardownCause::SHUTDOWN) {
      // Peer is gone or misbehaving: drop queued output.
      while (!s.control.empty()) s.control.pop();
      s.data_queue.clear();
      s.pending_out.reset();
    }
    return;
  }
}

void GatewayNode::begin_shutdown(MonoNs now) {
  shutting_down_ = true;
  for (auto& [key, s] : sessions_) {
    (void)key;
    if (!s.dead) teardown_session(s.id, TeardownCause::SHUTDOWN, now);
  }
}

// ---------------------------------------------------------------------------
// Pump / emission (ADR-0008: control -> retransmit -> new data)
// ---------------------------------------------------------------------------

bool GatewayNode::control_rate_admit(const Endpoint& peer, MonoNs now) {
  constexpr std::size_t kMaxTrackedEndpoints = 1024;
  auto it = rate_limits_.find(peer);
  if (it == rate_limits_.end()) {
    if (rate_limits_.size() >= kMaxTrackedEndpoints) {
      counters_.control_rate_dropped++;
      return false;
    }
    RateState rs;
    rs.bucket.configure(100, 32);  // ADR-0013
    rs.last_seen = now;
    it = rate_limits_.emplace(peer, std::move(rs)).first;
  }
  it->second.last_seen = now;
  it->second.bucket.refill(now);
  if (it->second.bucket.try_take(1)) return true;
  counters_.control_rate_dropped++;
  return false;
}

bool GatewayNode::pump(INetwork& net, MonoNs now) {
  bool pending = false;

  auto send_one = [&](Datagram& dg, Session* s) -> int {
    // 1 = sent, 0 = pending (EAGAIN), -1 = stop/error
    SendResult r = net.send(dg.peer, dg.bytes);
    switch (r.status) {
      case SendStatus::SENT:
        if (s != nullptr) s->note_outbound(now);
        return 1;
      case SendStatus::WOULD_BLOCK:
        if (s != nullptr) {
          s->pending_out = std::move(dg);
          s->packets.send_would_block++;
        } else {
          node_pending_ = std::move(dg);
        }
        return 0;
      default:
        // UNREACHABLE/REFUSED/MESSAGE_TOO_LARGE/FATAL: bounded teardown
        // (ADR-0009). EINTR never surfaces (adapter retries).
        if (s != nullptr) {
          counters_.send_errors++;
          teardown_session(s->id, TeardownCause::SOCKET_ERROR, now);
        } else {
          counters_.send_errors++;
        }
        return -1;
    }
  };

  // Node-level control (PONG for unknown/unsynced peers).
  if (node_pending_) {
    Datagram dg = std::move(*node_pending_);
    node_pending_.reset();
    if (send_one(dg, nullptr) == 0) pending = true;
  }
  while (!node_control_.empty()) {
    node_control_tokens_.refill(now);
    const Datagram& front = node_control_.front();
    if (!node_control_tokens_.try_take(
            static_cast<std::int64_t>(front.bytes.size()))) {
      pending = true;
      break;
    }
    Datagram dg = node_control_.pop();
    const int r = send_one(dg, nullptr);
    if (r == 0) {
      pending = true;
      break;
    }
    if (r < 0) break;
  }

  // Round-robin sessions from the rotating cursor (ADR-0008).
  const std::size_t n = sessions_.size();
  if (n > 0) {
    auto start = sessions_.begin();
    std::advance(start, static_cast<std::ptrdiff_t>(pump_cursor_ % n));
    auto it = start;
    for (std::size_t visited = 0; visited < n; ++visited) {
      Session& s = it->second;
      it = std::next(it) == sessions_.end() ? sessions_.begin()
                                            : std::next(it);
      if (s.dead) continue;
      pump_session(s, net, now, send_one);
      if (s.pending_out) pending = true;
    }
    pump_cursor_ = (pump_cursor_ + 1) % n;
  }
  return pending;
}

void GatewayNode::pump_session(
    Session& s, INetwork&, MonoNs now,
    const std::function<int(Datagram&, Session*)>& send_one) {
  s.data_tokens.refill(now);
  s.control_tokens.refill(now);
  std::int64_t bytes_left = cfg_.turn_byte_limit;
  std::int64_t count_left = cfg_.turn_datagram_limit;

  // 0. Pending datagram from a previous EAGAIN (ADR-0009).
  if (s.pending_out) {
    Datagram dg = std::move(*s.pending_out);
    s.pending_out.reset();
    const int r = send_one(dg, &s);
    if (r != 1) return;
  }
  if (s.dead) return;

  // 1. Control traffic — PONG, PING, ABANDON (priority over DATA).
  while (!s.control.empty() && count_left > 0) {
    s.control_tokens.refill(now);
    const Datagram& front = s.control.front();
    if (!s.control_tokens.try_take(
            static_cast<std::int64_t>(front.bytes.size()))) {
      break;  // control bucket drained; refill next turn
    }
    Datagram dg = s.control.pop();
    const int r = send_one(dg, &s);
    if (r != 1) return;
    count_left--;
    bytes_left -= static_cast<std::int64_t>(dg.bytes.size());
  }

  // 2. Retransmitted DATA (priority over new DATA).
  while (!s.data_queue.empty() && count_left > 0) {
    Datagram& front = s.data_queue.front();
    if (!s.data_tokens.try_take(
            static_cast<std::int64_t>(front.bytes.size()))) {
      break;
    }
    Datagram dg = std::move(front);
    s.data_queue.pop_front();
    const int r = send_one(dg, &s);
    if (r != 1) return;
    count_left--;
    bytes_left -= static_cast<std::int64_t>(dg.bytes.size());
  }

  // 3. New DATA within the remaining turn and token budgets.
  while (count_left > 0) {
    const std::int64_t budget =
        std::min<std::int64_t>(bytes_left, s.data_tokens.tokens());
    if (budget < 64) break;  // below one minimum datagram
    auto dg = s.sender.emit_new_data(now, budget);
    if (!dg) break;
    const int r = send_one(*dg, &s);
    if (r != 1) return;
    count_left--;
    bytes_left -= static_cast<std::int64_t>(dg->bytes.size());
  }
}

// ---------------------------------------------------------------------------
// Observability
// ---------------------------------------------------------------------------

std::vector<SessionMetrics> GatewayNode::collect_sessions() const {
  std::vector<SessionMetrics> out;
  out.reserve(sessions_.size());
  for (const auto& [key, s] : sessions_) {
    (void)key;
    SessionMetrics m;
    m.session_id = s.id;
    m.generation = s.generation;
    m.stream_id = s.stream_id;
    m.endpoint = s.peer;
    m.state = s.dead ? "dead" : "established";
    m.teardown_cause = s.teardown_cause;
    m.srtt_ns = static_cast<std::uint64_t>(s.sender.rtt().srtt());
    m.rto_ns = static_cast<std::uint64_t>(s.sender.rtt().current());
    m.window_packets_in_flight = s.sender.allocated_unacked();
    m.frames = s.frames;
    m.packets = s.packets;
    m.packets += s.sender.counters();
    m.keepalive_probes_unanswered = s.unanswered_probes();
    out.push_back(std::move(m));
  }
  return out;
}

SessionFrameLedger GatewayNode::aggregate_ledger() const {
  SessionFrameLedger agg;
  for (const auto& [key, s] : sessions_) {
    (void)key;
    agg.ontime += s.frames.ontime;
    agg.late += s.frames.late;
    agg.abandoned += s.frames.abandoned;
    agg.expired += s.frames.expired;
    agg.pre_admission_dropped += s.frames.pre_admission_dropped;
    agg.in_flight += s.frames.in_flight;
  }
  return agg;
}

std::size_t GatewayNode::control_hwm() const { return control_hwm_; }

bool GatewayNode::output_drained() const {
  if (!node_control_.empty() || node_pending_) return false;
  for (const auto& [key, s] : sessions_) {
    (void)key;
    if (!s.control.empty() || !s.data_queue.empty() || s.pending_out) {
      return false;
    }
  }
  return true;
}

std::vector<std::pair<std::uint32_t, std::uint32_t>>
GatewayNode::live_sessions() const {
  std::vector<std::pair<std::uint32_t, std::uint32_t>> out;
  for (const auto& [key, s] : sessions_) {
    (void)key;
    if (!s.dead) out.push_back({s.id, s.generation});
  }
  return out;
}

}  // namespace rudp_rt
