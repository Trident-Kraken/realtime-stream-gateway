// session.cpp — Session-side client core: ClientNode (ADR-0015) plus shared
// session helpers. The client implements the receiver side of the protocol:
// Join/retry, PONG sync, ACK-per-DATA, keepalive, ABANDON application, and
// playout accounting.

#include "rudp_rt/session.hpp"

#include <algorithm>
#include <memory>
#include <random>

namespace rudp_rt {

namespace {

std::function<std::uint32_t()> default_client_rng() {
  auto engine = std::make_shared<std::mt19937>(std::random_device{}());
  return [engine]() { return (*engine)(); };
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

constexpr MonoNs kJoinRetryInterval = ns_from_ms(200);  // ADR-0015

std::span<const std::byte> payload_of(const Datagram& dg) {
  return std::span<const std::byte>(dg.bytes).subspan(kHeaderSize);
}

}  // namespace

// ---------------------------------------------------------------------------
// ClientNode
// ---------------------------------------------------------------------------

ClientNode::ClientNode(const Config& cfg, IClock& clock, Endpoint gateway,
                       std::uint16_t stream_id,
                       std::function<std::uint32_t()> rng)
    : cfg_(cfg),
      clock_(clock),
      gateway_(gateway),
      stream_id_(stream_id),
      rng_(rng ? std::move(rng) : default_client_rng()),
      playout_(ReceiverConfig{gateway,
                              stream_id,
                              0,
                              0,
                              cfg.window_w,
                              cfg.reorder_buffer_packets,
                              cfg.srtt_initial},
               128,
               cfg.jitter_buffer_frames,
               cfg.playout_delay,
               cfg.consumer_queue_frames,
               clock),
      control_(cfg.control_queue_datagrams) {
  rtt_.configure(cfg.srtt_initial, cfg.rttvar_initial, cfg.rto_min,
                 cfg.rto_max);
  tokens_.configure(cfg.token_rate_bytes_per_sec, cfg.control_reserve_bytes);
}

void ClientNode::start_join(MonoNs now) {
  client_epoch_ = rng_();  // fresh epoch per join (retries reuse it, ADR-0015)
  state_ = State::JOINING;
  send_join(now);
}

void ClientNode::send_join(MonoNs now) {
  Ping ping;
  ping.nonce = nonce_counter_++;
  ping.purpose = PingPurpose::JOIN;
  ping.client_epoch = client_epoch_;
  join_nonce_ = ping.nonce;
  last_join_send_ = now;
  Datagram dg;
  dg.peer = gateway_;
  const std::size_t len = encoded_ping_size(ping.purpose);
  dg.bytes.assign(kHeaderSize + len, std::byte{0});
  encode_header(dg.bytes.data(),
                control_header(stream_id_, PacketType::PING,
                               static_cast<std::uint16_t>(len)));
  encode_ping(dg.bytes.data() + kHeaderSize, ping);
  if (!control_.push(std::move(dg))) counters_.control_queue_dropped++;
  counters_.pings_sent++;
  note_outbound(now);
}

void ClientNode::queue_pong(std::uint32_t nonce, PongStatus status) {
  Pong pong;
  pong.nonce = nonce;
  pong.status = status;
  pong.session_epoch = session_epoch_;
  pong.server_isn = server_isn_;
  Datagram dg;
  dg.peer = gateway_;
  dg.bytes.assign(kHeaderSize + encoded_pong_size(), std::byte{0});
  encode_header(dg.bytes.data(),
                control_header(stream_id_, PacketType::PONG,
                               static_cast<std::uint16_t>(encoded_pong_size())));
  encode_pong(dg.bytes.data() + kHeaderSize, pong);
  if (!control_.push(std::move(dg))) counters_.control_queue_dropped++;
  counters_.pongs_sent++;
}

void ClientNode::on_datagram(const Datagram& dg, MonoNs now) {
  auto decoded = decode_header(dg.bytes);
  if (decoded.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  const Header& h = decoded.header;
  const auto type = static_cast<PacketType>(h.type);

  switch (type) {
    case PacketType::DATA:
      handle_data(dg, h, now);
      return;
    case PacketType::PING:
      handle_ping(dg, h, now);
      return;
    case PacketType::PONG:
      handle_pong(dg, h, now);
      return;
    case PacketType::ABANDON:
      handle_abandon(dg, h, now);
      return;
    case PacketType::ACK:
    case PacketType::NACK:
      // Server -> client direction never carries these.
      counters_.stale_control++;
      return;
  }
}

void ClientNode::handle_data(const Datagram& dg, const Header& h, MonoNs now) {
  if (h.stream_id != stream_id_) {
    counters_.stream_mismatch++;
    return;
  }
  route_control(playout_.on_data(dg, now));
  last_inbound_ = now;
}

void ClientNode::handle_ping(const Datagram& dg, const Header&, MonoNs now) {
  auto ping = decode_ping(payload_of(dg));
  if (ping.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  counters_.pings_received++;
  if (!established()) {
    counters_.stale_control++;
    return;
  }
  // The gateway probes with KEEPALIVE; answer PONG with the session epoch.
  queue_pong(ping.ping.nonce, PongStatus::OK);
  note_outbound(now);
}

void ClientNode::handle_pong(const Datagram& dg, const Header&, MonoNs now) {
  auto pong = decode_pong(payload_of(dg));
  if (pong.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  counters_.pongs_received++;
  const Pong& p = pong.pong;

  if (state_ == State::JOINING && p.nonce == join_nonce_) {
    if (p.status == PongStatus::OK) {
      session_epoch_ = p.session_epoch;
      server_isn_ = p.server_isn;
      playout_.set_sync(p.server_isn, p.session_epoch);
      state_ = State::ESTABLISHED;
      rtt_.sample(now - last_join_send_);
      last_probe_send_ = now;
      last_outbound_ = now;
      unanswered_probes_ = 0;
    } else {
      rejects_received++;
      last_reject = p.status;  // retry continues on the join timer
    }
    return;
  }
  if (state_ == State::ESTABLISHED && p.nonce == probe_nonce_ &&
      probe_nonce_ != 0) {
    if (p.session_epoch != session_epoch_) {
      counters_.stale_control++;
      return;
    }
    unanswered_probes_ = 0;
    rtt_.sample(now - last_probe_send_);
    playout_.set_nack_throttle(rtt_.srtt());
    return;
  }
  counters_.stale_control++;
}

void ClientNode::handle_abandon(const Datagram& dg, const Header& h,
                                MonoNs now) {
  if (h.stream_id != stream_id_) {
    counters_.stream_mismatch++;
    return;
  }
  auto ab = decode_abandon(payload_of(dg));
  if (ab.reject != RejectReason::NONE) {
    counters_.malformed++;
    return;
  }
  if (!established() || ab.ab.session_epoch != session_epoch_) {
    counters_.stale_control++;
    return;
  }
  counters_.abandons_received++;
  route_control(playout_.apply_abandon(std::move(ab.ab.ranges), now));
  note_outbound(now);
}

void ClientNode::route_control(std::vector<Datagram> dgs) {
  for (auto& dg : dgs) {
    if (!control_.push(std::move(dg))) {
      counters_.control_queue_dropped++;
    } else {
      control_.observe_hwm();
    }
  }
}

void ClientNode::check_keepalive(MonoNs now) {
  if (!established()) return;
  const MonoNs silence = now - last_outbound_;
  if (silence >= cfg_.keepalive_silence_after &&
      now - last_probe_send_ >= cfg_.keepalive_min_interval) {
    Ping ping;
    ping.nonce = nonce_counter_++;
    ping.purpose = PingPurpose::KEEPALIVE;
    ping.session_epoch = session_epoch_;
    ping.client_epoch = client_epoch_;
    probe_nonce_ = ping.nonce;
    last_probe_send_ = now;
    unanswered_probes_++;
    Datagram dgp;
    dgp.peer = gateway_;
    const std::size_t len = encoded_ping_size(ping.purpose);
    dgp.bytes.assign(kHeaderSize + len, std::byte{0});
    encode_header(dgp.bytes.data(),
                  control_header(stream_id_, PacketType::PING,
                                 static_cast<std::uint16_t>(len)));
    encode_ping(dgp.bytes.data() + kHeaderSize, ping);
    if (!control_.push(std::move(dgp))) counters_.control_queue_dropped++;
    counters_.pings_sent++;
    note_outbound(now);
  }
  if (unanswered_probes_ >=
      static_cast<std::uint32_t>(cfg_.keepalive_max_probes)) {
    // Session timeout on the client side (spec §4.3).
    playout_.teardown();
    state_ = State::DOWN;
    if (cfg_.auto_rejoin) {
      start_join(clock_.now());
    }
  }
}

void ClientNode::on_timer(MonoNs now) {
  if (torn_down_) return;
  if (state_ == State::JOINING &&
      now - last_join_send_ >= kJoinRetryInterval) {
    send_join(now);
  }
  if (state_ == State::ESTABLISHED) {
    check_keepalive(now);
    route_control(playout_.on_timer(now));
  }
}

std::optional<MonoNs> ClientNode::next_deadline() const {
  std::optional<MonoNs> best;
  const auto consider = [&best](MonoNs t) {
    if (!best || t < *best) best = t;
  };
  if (state_ == State::JOINING) {
    consider(last_join_send_ + kJoinRetryInterval);
  }
  if (state_ == State::ESTABLISHED) {
    consider(std::max(last_outbound_ + cfg_.keepalive_silence_after,
                      last_probe_send_ + cfg_.keepalive_min_interval));
    if (auto t = playout_.next_deadline()) consider(*t);
  }
  return best;
}

void ClientNode::note_outbound(MonoNs now) {
  last_outbound_ = now;
}

bool ClientNode::pump(INetwork& net, MonoNs now) {
  tokens_.refill(now);
  bool pending = false;
  std::int64_t count_left = cfg_.turn_datagram_limit;

  auto send_one = [&](Datagram& dg) -> int {
    SendResult r = net.send(dg.peer, dg.bytes);
    switch (r.status) {
      case SendStatus::SENT:
        note_outbound(now);
        return 1;
      case SendStatus::WOULD_BLOCK:
        pending_out_ = std::move(dg);
        counters_.send_would_block++;
        return 0;
      default:
        counters_.send_errors++;
        return -1;  // client keeps running; the join retry recovers
    }
  };

  if (pending_out_) {
    Datagram dg = std::move(*pending_out_);
    pending_out_.reset();
    if (send_one(dg) == 0) pending = true;
  }
  while (!control_.empty() && count_left > 0) {
    tokens_.refill(now);
    const Datagram& front = control_.front();
    if (!tokens_.try_take(static_cast<std::int64_t>(front.bytes.size()))) {
      pending = true;
      break;
    }
    Datagram dg = control_.pop();
    const int r = send_one(dg);
    if (r != 1) {
      pending = r == 0;
      break;
    }
    count_left--;
  }
  return pending;
}

std::vector<JitterBuffer::Delivery> ClientNode::take_deliveries() {
  return playout_.take_deliveries();
}

void ClientNode::shutdown(MonoNs) { torn_down_ = true; }

std::uint64_t ClientNode::bytes_owned() const { return 0; }

}  // namespace rudp_rt
