// sender.cpp — TxFrame/TxPacket state machine (spec §4.4–§4.6, ADR-0011).

#include "rudp_rt/sender.hpp"

#include <algorithm>

namespace rudp_rt {

namespace {

// Ceiling-saturated remaining lifetime in milliseconds (ADR-0004).
std::uint16_t remaining_ms(MonoNs deadline_abs, MonoNs now) {
  const MonoNs rem = deadline_abs - now;
  if (rem <= 0) return 0;
  MonoNs ms = (rem + kNsPerMs - 1) / kNsPerMs;
  if (ms > 65535) ms = 65535;
  return static_cast<std::uint16_t>(ms);
}

}  // namespace

// ---------------------------------------------------------------------------
// RtoEstimator (RFC 6298-style; ADR-0011)
// ---------------------------------------------------------------------------

void RtoEstimator::configure(MonoNs srtt_initial, MonoNs rttvar_initial,
                             MonoNs rto_min, MonoNs rto_max) {
  srtt_ = srtt_initial;
  rttvar_ = rttvar_initial;
  rto_min_ = rto_min;
  rto_max_ = rto_max;
  has_sample_ = false;
}

void RtoEstimator::sample(MonoNs r) {
  if (!has_sample_) {
    srtt_ = r;
    rttvar_ = r / 2;
    has_sample_ = true;
    return;
  }
  const MonoNs diff = r > srtt_ ? r - srtt_ : srtt_ - r;
  rttvar_ = (3 * rttvar_ + diff) / 4;       // 0.75 * RTTVAR + 0.25 * |SRTT-R|
  srtt_ = (7 * srtt_ + r) / 8;              // 0.875 * SRTT + 0.125 * R
}

MonoNs RtoEstimator::current() const {
  const MonoNs g = kNsPerMs;  // G = 1 ms (v0.2 §3.12)
  MonoNs rto = srtt_ + std::max(g, 4 * rttvar_);
  return std::clamp(rto, rto_min_, rto_max_);
}

// ---------------------------------------------------------------------------
// TransportSender
// ---------------------------------------------------------------------------

TransportSender::TransportSender(SenderConfig cfg, IClock& clock)
    : cfg_(cfg), clock_(&clock) {
  rtt_.configure(cfg.srtt_initial, cfg.rttvar_initial, cfg.rto_min,
                 cfg.rto_max);
  send_base_ = cfg.isn;
  next_seq_ = cfg.isn;
  last_valid_ack_ = cfg.isn - 1;
}

void TransportSender::on_frame(MediaFrame frame) {
  const MonoNs now = clock_->now();
  if (torn_down_) {
    out_.admissions.push_back({frame.frame_id, false});
    return;
  }
  auto pf = packetizer_.packetize(std::move(frame), cfg_.ssrc,
                                  cfg_.media_payload_bytes,
                                  cfg_.max_frame_packets);
  if (!pf) {
    counters_.protocol_violations++;  // frame larger than configuration
    out_.admissions.push_back({pf ? 0u : 0u, false});
    return;
  }
  // Broken GOP Fast Pruning: If this is a DELTA_FRAME whose dependency was
  // already abandoned, drop it before admission to save window slots.
  if (pf->frame_type == FrameType::DELTA_FRAME &&
      is_dependency_abandoned(pf->depends_on_frame)) {
    counters_.cascade_abandons++;
    record_abandoned_frame(pf->frame_id);
    out_.admissions.push_back({pf->frame_id, false});
    return;
  }
  const MonoNs deadline_abs = pf->capture_time + pf->deadline_budget;
  const MonoNs remaining = deadline_abs - now;
  // Early-exit threshold capped at D/2 (ADR-0017): prevents ACK-HOLB RTT
  // inflation under loss from starving all admissions.
  const MonoNs threshold =
      std::min(rtt_.admission_threshold(), pf->deadline_budget / 2);
  if (remaining <= threshold) {
    out_.admissions.push_back({pf->frame_id, false});
    return;
  }
  const std::uint64_t allocated =
      seq_forward(send_base_, next_seq_) + pf->frame_count();
  if (allocated > static_cast<std::uint64_t>(cfg_.window_w)) {
    out_.admissions.push_back({pf->frame_id, false});
    return;
  }

  TxFrame f;
  f.frame_id = pf->frame_id;
  f.rtp_timestamp = pf->rtp_timestamp;
  f.stream_id = pf->stream_id;
  f.ssrc = pf->ssrc;
  f.deadline_abs = deadline_abs;
  f.frame_type = pf->frame_type;
  f.depends_on_frame = pf->depends_on_frame;
  f.frame_count = pf->frame_count();
  f.first_seq = next_seq_;
  f.media = std::move(pf->bytes);
  f.sync_start = sync_start_pending_;
  sync_start_pending_ = false;

  f.packets.resize(f.frame_count);
  for (std::uint16_t i = 0; i < f.frame_count; ++i) {
    auto& p = f.packets[i];
    p.seq = next_seq_ + i;
    p.frame_index = i;
    p.rtp_seq = pf->fragments[i].rtp_seq;
    p.offset = pf->fragments[i].offset;
    p.len = pf->fragments[i].len;
    p.frame_end = pf->fragments[i].frame_end;
    p.first_send = -1;
    p.last_send = -1;
  }
  next_seq_ += f.frame_count;
  media_bytes_ += f.media.size();
  frames_.push_back(std::move(f));
  any_frame_admitted_ = true;
  out_.admissions.push_back({frames_.back().frame_id, true});
}

void TransportSender::on_ack(const Ack& ack, MonoNs now) {
  if (ack.session_epoch != cfg_.session_epoch) {
    counters_.stale_control++;
    return;
  }
  // Monotonicity + allocation-domain validation (spec §4.5).
  if (seq_lt(ack.ack_number, last_valid_ack_)) {
    counters_.stale_control++;  // stale
    return;
  }
  if (seq_gt(ack.ack_number, seq_prev(next_seq_))) {
    counters_.stale_control++;  // future / across unallocated sequence
    return;
  }
  counters_.acks_received++;

  if (ack.ack_number == last_valid_ack_) {
    // Duplicate cumulative value: SACK bits may still add information.
    apply_sack_bits(ack);
    return;
  }

  // RTT sampling (ADR-0018): only a single-packet cumulative advance yields
  // a sample — in-order consumption is never delayed by hole repair, so the
  // sample tracks the path RTT instead of recovery latency. Multi-packet
  // advances (hole recoveries) and SACK-only ACKs produce no sample; Karn
  // excludes retransmitted packets.
  if (ack.ack_number == seq_next(last_valid_ack_)) {
    TxFrame* f = find_frame_for_seq(ack.ack_number);
    if (f != nullptr) {
      for (const auto& p : f->packets) {
        if (p.seq == ack.ack_number) {
          if (p.retransmissions == 0 && p.first_send >= 0) {
            rtt_.sample(now - p.first_send);
            counters_.rtt_samples++;
          }
          break;
        }
      }
    }
  }

  last_valid_ack_ = ack.ack_number;
  send_base_ = seq_next(ack.ack_number);
  apply_sack_bits(ack);
  pop_covered_frames(now);
}

void TransportSender::apply_sack_bits(const Ack& ack) {
  for (std::uint32_t i = 0; i < 64; ++i) {
    if ((ack.sack_bitmap & (1ull << i)) == 0) continue;
    const std::uint32_t seq = seq_advance(seq_next(ack.ack_number), i);
    TxFrame* f = find_frame_for_seq(seq);
    if (f == nullptr) continue;
    for (auto& p : f->packets) {
      if (p.seq == seq) {
        if (!p.sacked) {
          p.sacked = true;
          counters_.sacks_reported++;
        }
        break;
      }
    }
  }
}

void TransportSender::on_nack(const Nack& nack, MonoNs now) {
  if (nack.session_epoch != cfg_.session_epoch) {
    counters_.stale_control++;
    return;
  }
  counters_.nacks_received++;
  for (const std::uint32_t seq : nack.seqs) {
    if (seq_lt(seq, send_base_)) continue;  // already consumed
    TxFrame* f = find_frame_for_seq(seq);
    if (f == nullptr) continue;  // out of window / unknown
    if (f->abandoned) {
      queue_abandon_send(*f, now);
      continue;
    }
    TxPacket* target = nullptr;
    for (auto& p : f->packets) {
      if (p.seq == seq) {
        target = &p;
        break;
      }
    }
    if (target == nullptr) continue;
    if (target->sacked) continue;  // receiver claims to hold it
    if (target->in_flight && now - target->last_send < rtt_.srtt()) {
      continue;  // NACK suppression window (ADR-0011)
    }
    if (target->in_flight) {
      retransmit_packet(*f, *target, now);
    } else {
      // Never-sent packet named by a NACK (receiver derived the interval):
      // send it now with first-send semantics.
      Datagram dg = build_data(*f, *target, false);
      target->first_send = now;
      target->last_send = now;
      target->in_flight = true;
      counters_.data_sent++;
      out_.data.push_back(std::move(dg));
    }
  }
}

void TransportSender::retransmit_packet(TxFrame& f, TxPacket& p, MonoNs now) {
  p.retransmissions++;
  p.last_send = now;
  Datagram dg = build_data(f, p, true);
  counters_.data_retransmissions++;
  out_.data.push_back(std::move(dg));
}

void TransportSender::on_timer(MonoNs now) {
  if (torn_down_) return;
  std::vector<AbandonRange> pending_ranges;

  for (auto& f : frames_) {
    if (f.abandoned) {
      if (f.last_abandon_send >= 0 &&
          now - f.last_abandon_send >= effective_rto(f.abandon_retries)) {
        f.last_abandon_send = now;
        f.abandon_retries++;
        counters_.abandon_retransmissions++;
        pending_ranges.push_back(
            {f.first_seq, seq_advance(f.first_seq, f.frame_count - 1)});
      }
      continue;
    }

    if (cfg_.deadline_mode) {
      // When the receiver already holds every packet (ADR-0011 amendment),
      // grant one RTO of grace for the in-flight cumulative ACK before
      // abandoning; this keeps the scan deadline strictly in the future
      // (no busy waiting) and cannot resurrect the frame (INV-07).
      bool all_held = true;
      for (const auto& p : f.packets) {
        if (!p.sacked) {
          all_held = false;
          break;
        }
      }
      const MonoNs abandon_at =
          f.deadline_abs + (all_held ? effective_rto(0) : 0);
      if (now >= abandon_at) {
        abandon_frame(f, now);
        pending_ranges.push_back(
            {f.first_seq, seq_advance(f.first_seq, f.frame_count - 1)});
        cascade_abandon_dependents(f.frame_id, now, pending_ranges);
        continue;
      }
    }

    for (auto& p : f.packets) {
      if (!p.in_flight || p.sacked) continue;
      if (now - p.last_send >= effective_rto(p.retransmissions)) {
        retransmit_packet(f, p, now);
      }
    }
  }

  if (!pending_ranges.empty()) {
    emit_abandon_ranges(normalize_abandon_ranges(pending_ranges));
  }
}

void TransportSender::record_abandoned_frame(std::uint32_t fid) {
  if (abandoned_frame_ids_.size() >= 256) {
    abandoned_frame_ids_.pop_front();
  }
  for (std::uint32_t x : abandoned_frame_ids_) {
    if (x == fid) return;
  }
  abandoned_frame_ids_.push_back(fid);
}

bool TransportSender::is_dependency_abandoned(std::uint32_t fid) const {
  for (std::uint32_t x : abandoned_frame_ids_) {
    if (x == fid) return true;
  }
  return false;
}

void TransportSender::cascade_abandon_dependents(
    std::uint32_t root_abandoned_frame_id, MonoNs now,
    std::vector<AbandonRange>& pending_ranges) {
  record_abandoned_frame(root_abandoned_frame_id);
  out_.keyframe_requested = true;
  std::vector<std::uint32_t> newly_abandoned{root_abandoned_frame_id};
  bool found_any = true;
  while (found_any) {
    found_any = false;
    for (auto& f : frames_) {
      if (f.abandoned || f.closed) continue;
      bool depends = false;
      for (std::uint32_t fid : newly_abandoned) {
        if (f.frame_type == FrameType::DELTA_FRAME && f.depends_on_frame == fid) {
          depends = true;
          break;
        }
      }
      if (depends) {
        abandon_frame(f, now);
        pending_ranges.push_back(
            {f.first_seq, seq_advance(f.first_seq, f.frame_count - 1)});
        counters_.cascade_abandons++;
        record_abandoned_frame(f.frame_id);
        newly_abandoned.push_back(f.frame_id);
        found_any = true;
      }
    }
  }
}

void TransportSender::abandon_frame(TxFrame& f, MonoNs now) {
  f.abandoned = true;
  f.last_abandon_send = now;
  f.abandon_retries = 0;
  record_abandoned_frame(f.frame_id);
  release_frame_media(f);
  close_frame(f, SessionFrameOutcome::ABANDONED, now);
}

void TransportSender::queue_abandon_send(TxFrame& f, MonoNs now) {
  if (f.last_abandon_send >= 0 &&
      now - f.last_abandon_send < effective_rto(f.abandon_retries)) {
    return;  // throttle
  }
  f.last_abandon_send = now;
  f.abandon_retries++;
  counters_.abandon_retransmissions++;
  std::vector<AbandonRange> ranges{
      {f.first_seq, seq_advance(f.first_seq, f.frame_count - 1)}};
  emit_abandon_ranges(normalize_abandon_ranges(ranges));
}

void TransportSender::emit_abandon_ranges(std::vector<AbandonRange> ranges) {
  std::size_t i = 0;
  while (i < ranges.size()) {
    const std::size_t chunk = std::min<std::size_t>(kMaxAbandonRanges,
                                                    ranges.size() - i);
    EncodedAbandon enc;
    enc.session_epoch = cfg_.session_epoch;
    enc.ranges.assign(ranges.begin() + static_cast<std::ptrdiff_t>(i),
                      ranges.begin() + static_cast<std::ptrdiff_t>(i + chunk));
    Datagram dg;
    dg.peer = cfg_.peer;
    dg.bytes.assign(kHeaderSize + encoded_abandon_size(chunk), std::byte{0});
    Header h;
    h.version = kWireVersion;
    h.header_len = kHeaderLenWords;
    h.type = static_cast<std::uint8_t>(PacketType::ABANDON);
    h.stream_id = cfg_.stream_id;
    h.payload_len = static_cast<std::uint16_t>(encoded_abandon_size(chunk));
    encode_header(dg.bytes.data(), h);
    encode_abandon(dg.bytes.data() + kHeaderSize, enc);
    counters_.abandons_sent++;
    out_.control.push_back(std::move(dg));
    i += chunk;
  }
}

std::optional<Datagram> TransportSender::emit_new_data(
    MonoNs now, std::int64_t byte_budget) {
  if (torn_down_) return std::nullopt;
  for (auto& f : frames_) {
    if (f.abandoned) continue;
    for (auto& p : f.packets) {
      if (p.in_flight) continue;
      Datagram dg = build_data(f, p, false);
      if (static_cast<std::int64_t>(dg.bytes.size()) > byte_budget) {
        return std::nullopt;  // leave unsent; retry when tokens refill
      }
      p.first_send = now;
      p.last_send = now;
      p.in_flight = true;
      counters_.data_sent++;
      return dg;
    }
  }
  return std::nullopt;
}

std::optional<MonoNs> TransportSender::next_deadline() const {
  std::optional<MonoNs> best;
  const auto consider = [&best](MonoNs t) {
    if (!best || t < *best) best = t;
  };
  for (const auto& f : frames_) {
    if (f.abandoned) {
      if (f.last_abandon_send >= 0) {
        consider(f.last_abandon_send + effective_rto(f.abandon_retries));
      }
      continue;
    }
    if (cfg_.deadline_mode) {
      bool all_held = true;
      for (const auto& p : f.packets) {
        if (!p.sacked) {
          all_held = false;
          break;
        }
      }
      consider(f.deadline_abs +
               (all_held ? effective_rto(0) : 0));
    }
    for (const auto& p : f.packets) {
      if (p.in_flight && !p.sacked) {
        consider(p.last_send + effective_rto(p.retransmissions));
      }
    }
  }
  return best;
}

void TransportSender::teardown(TeardownCause cause, MonoNs now) {
  if (torn_down_) return;
  std::vector<AbandonRange> ranges;
  for (auto& f : frames_) {
    if (!f.closed) {
      close_frame(f, SessionFrameOutcome::ABANDONED, now);
    }
    if (cause == TeardownCause::SHUTDOWN) {
      ranges.push_back(
          {f.first_seq, seq_advance(f.first_seq, f.frame_count - 1)});
    }
    release_frame_media(f);
  }
  if (!ranges.empty()) emit_abandon_ranges(normalize_abandon_ranges(ranges));
  frames_.clear();
  media_bytes_ = 0;
  torn_down_ = true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

TxFrame* TransportSender::find_frame_for_seq(std::uint32_t seq) {
  for (auto& f : frames_) {
    if (seq_ge(seq, f.first_seq) &&
        seq_lt(seq, seq_advance(f.first_seq, f.frame_count))) {
      return &f;
    }
  }
  return nullptr;
}

MonoNs TransportSender::effective_rto(std::uint16_t retries) const {
  MonoNs rto = rtt_.current();
  const int shift = std::min<int>(retries, 16);
  for (int i = 0; i < shift; ++i) {
    rto = std::min<MonoNs>(rto * 2, cfg_.rto_max);
  }
  return std::clamp(rto, cfg_.rto_min, cfg_.rto_max);
}

Datagram TransportSender::build_data(const TxFrame& f, const TxPacket& p,
                                     bool retransmit) const {
  Header h;
  h.stream_id = f.stream_id;
  h.seq = p.seq;
  h.frame_id = f.frame_id;
  h.rtp_timestamp = f.rtp_timestamp;
  h.deadline_ms = remaining_ms(f.deadline_abs, clock_->now());
  h.frame_index = p.frame_index;
  h.frame_count = f.frame_count;
  h.flags = 0;
  if (retransmit) h.flags |= kFlagRetransmit;
  if (p.frame_end) h.flags |= kFlagFrameEnd;
  if (p.frame_index == 0 && f.sync_start) h.flags |= kFlagSyncStart;

  RtpHeader rtp;
  rtp.sequence = p.rtp_seq;
  rtp.timestamp = f.rtp_timestamp;
  rtp.ssrc = f.ssrc;
  rtp.marker = p.frame_end;

  const std::byte* base = f.media.data();
  auto frag = std::span<const std::byte>(base + p.offset, p.len);
  auto dg = build_data_datagram(cfg_.peer, h, rtp, frag);
  // build_data_datagram only fails on over-size fragments, impossible here
  // because the packetizer enforced the bound at admission.
  return dg ? std::move(*dg) : Datagram{};
}

void TransportSender::close_frame(TxFrame& f, SessionFrameOutcome outcome,
                                  MonoNs) {
  if (f.closed) return;
  f.closed = true;
  out_.closures.push_back({f.frame_id, outcome});
}

void TransportSender::release_frame_media(TxFrame& f) {
  media_bytes_ -= f.media.size();
  std::vector<std::byte>().swap(f.media);
}

void TransportSender::pop_covered_frames(MonoNs now) {
  while (!frames_.empty()) {
    TxFrame& front = frames_.front();
    const std::uint32_t last_seq =
        seq_advance(front.first_seq, front.frame_count - 1);
    if (!seq_le(last_seq, last_valid_ack_)) break;
    close_frame(front, now < front.deadline_abs ? SessionFrameOutcome::ONTIME
                                                : SessionFrameOutcome::LATE,
                now);
    release_frame_media(front);
    frames_.pop_front();
  }
}


}  // namespace rudp_rt
