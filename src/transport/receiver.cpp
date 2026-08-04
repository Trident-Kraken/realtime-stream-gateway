// receiver.cpp — receive window, reorder storage, ACK/NACK, ABANDON (spec
// §4.5, §4.7). Single-writer; classification order per spec §4.7.

#include "rudp_rt/receiver.hpp"

#include <algorithm>

namespace rudp_rt {

UdpReceiver::UdpReceiver(ReceiverConfig cfg, IClock& clock)
    : cfg_(cfg), clock_(clock) {}

void UdpReceiver::set_sync(std::uint32_t server_isn,
                           std::uint32_t session_epoch) {
  recv_next_ = server_isn;
  cfg_.session_epoch = session_epoch;
  synced_ = true;
  storage_.clear();
  hole_last_nack_.clear();
  abandon_ranges_.clear();
  dead_frames_.clear();
}

void UdpReceiver::set_nack_throttle(MonoNs srtt) {
  cfg_.nack_throttle_srtt = std::max<MonoNs>(srtt, kNsPerMs);
}

void UdpReceiver::teardown() {
  synced_ = false;
  storage_.clear();
  hole_last_nack_.clear();
  abandon_ranges_.clear();
  dead_frames_.clear();
  bytes_owned_ = 0;
}

bool UdpReceiver::frame_dead(std::uint32_t frame_id) const {
  return dead_frames_.count(frame_id) != 0;
}

void UdpReceiver::mark_frame_dead(std::uint32_t frame_id) {
  if (dead_frames_.size() >= 256) {
    // Bounded tombstones: drop the oldest by serial order relative to the
    // smallest key (frames arrive in increasing serial order in practice).
    dead_frames_.erase(dead_frames_.begin());
  }
  dead_frames_[frame_id] = 1;
}

void UdpReceiver::set_descriptor_hooks(
    std::function<DataClassification(const AcceptedPacket&)> validate,
    std::function<void(std::uint32_t)> kill_frame) {
  validate_descriptor_ = std::move(validate);
  kill_frame_ = std::move(kill_frame);
}

// ---------------------------------------------------------------------------
// DATA processing
// ---------------------------------------------------------------------------

ControlOutput UdpReceiver::on_datagram(const Datagram& datagram, MonoNs now) {
  ControlOutput out;
  auto decoded = decode_header(datagram.bytes);
  if (decoded.reject != RejectReason::NONE) {
    counters_.malformed++;
    return out;
  }
  const Header& h = decoded.header;
  if (static_cast<PacketType>(h.type) != PacketType::DATA) {
    counters_.malformed++;  // defensive: only DATA reaches the receiver
    return out;
  }
  if (h.stream_id != cfg_.stream_id) {
    counters_.stream_mismatch++;
    return out;
  }

  // RTP validation before commit (spec §4.1).
  const std::span<const std::byte> payload = datagram.bytes;
  auto rtp = decode_rtp_header(payload.subspan(kHeaderSize));
  if (rtp.reject != RejectReason::NONE) {
    counters_.malformed++;
    return out;
  }
  if (rtp.rtp.timestamp != h.rtp_timestamp) {
    counters_.malformed++;
    return out;
  }
  if (h.frame_count == 0 || h.frame_index >= h.frame_count) {
    counters_.protocol_violations++;
    return out;
  }
  const bool frame_end = (h.flags & kFlagFrameEnd) != 0;
  if (frame_end != (h.frame_index == h.frame_count - 1)) {
    counters_.protocol_violations++;  // FRAME_END ⇔ last index (v0.2 §3.11)
    return out;
  }
  if (!synced_) {
    counters_.pre_sync_dropped++;
    return out;
  }

  const std::uint32_t seq = h.seq;
  if (dead_frames_.count(h.frame_id) != 0) {
    counters_.dead_frame_drops++;
    return out;
  }
  if (seq_lt(seq, recv_next_)) {
    counters_.too_old++;
    return out;
  }
  if (!seq_lt(seq, recv_next_ + static_cast<std::uint32_t>(cfg_.window_w))) {
    counters_.out_of_window++;
    return out;
  }
  if (storage_.count(seq) != 0) {
    counters_.duplicates++;
    return out;
  }

  // Build the accepted candidate and run descriptor validation before any
  // storage mutation (spec §4.7 classification order).
  AcceptedPacket ap;
  ap.seq = seq;
  ap.frame_id = h.frame_id;
  ap.rtp_timestamp = h.rtp_timestamp;
  ap.stream_id = h.stream_id;
  ap.frame_index = h.frame_index;
  ap.frame_count = h.frame_count;
  ap.rtp_seq = rtp.rtp.sequence;
  ap.retransmit = (h.flags & kFlagRetransmit) != 0;
  ap.frame_end = frame_end;
  ap.sync_start = (h.flags & kFlagSyncStart) != 0;
  ap.deadline_ms = h.deadline_ms;
  ap.arrival = now;
  ap.media.assign(payload.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + kRtpHeaderSize),
                  payload.end());

  if (validate_descriptor_) {
    const DataClassification cls = validate_descriptor_(ap);
    if (cls != DataClassification::VALID) {
      counters_.protocol_violations++;
      mark_frame_dead(h.frame_id);
      if (kill_frame_) kill_frame_(h.frame_id);
      return out;
    }
  }

  if (static_cast<int>(storage_.size()) >= cfg_.reorder_capacity) {
    counters_.storage_full_drops++;  // drop newest unreported (D-12)
    return out;
  }

  storage_[seq] = StoredPacket{h.frame_id, h.frame_index, now, false};
  out.accepted.push_back(std::move(ap));
  counters_.data_received++;

  advance_recv_next(now, out);
  if (seq_gt(seq, recv_next_)) {
    // Out-of-order acceptance: per-hole NACKs, throttled (spec §4.5).
    enqueue_hole_nacks(seq, now, out.datagrams);
  }
  enqueue_ack(out.datagrams);
  prune_hole_state();
  return out;
}

bool UdpReceiver::advance_recv_next(MonoNs, ControlOutput& out) {
  bool moved = false;
  while (true) {
    const bool held = storage_.count(recv_next_) != 0;
    const bool abandoned = seq_covered_by_abandon(recv_next_);
    if (!held && !abandoned) break;
    if (held) storage_.erase(recv_next_);
    ++recv_next_;
    moved = true;
  }
  if (moved) {
    // Fresh ACK after any advancement (spec §4.5).
    enqueue_ack(out.datagrams);
  }
  return moved;
}

bool UdpReceiver::seq_covered_by_abandon(std::uint32_t seq) const {
  for (const auto& r : abandon_ranges_) {
    if (seq_ge(seq, r.start) && seq_le(seq, r.end)) return true;
  }
  return false;
}

void UdpReceiver::prune_hole_state() {
  for (auto it = hole_last_nack_.begin(); it != hole_last_nack_.end();) {
    if (seq_lt(it->first, recv_next_) || storage_.count(it->first) != 0) {
      it = hole_last_nack_.erase(it);
    } else {
      ++it;
    }
  }
}

void UdpReceiver::enqueue_ack(std::vector<Datagram>& out) {
  Ack ack;
  ack.ack_number = recv_next_ - 1;  // last cumulatively consumed (spec §4.4)
  for (const auto& [seq, entry] : storage_) {
    const std::uint64_t offset = seq_forward(seq_next(ack.ack_number), seq);
    if (offset < 64) {
      ack.sack_bitmap |= (1ull << offset);
    }
  }
  ack.session_epoch = cfg_.session_epoch;
  Datagram dg;
  dg.peer = cfg_.peer;
  dg.bytes.assign(kHeaderSize + encoded_ack_size(), std::byte{0});
  Header h;
  h.version = kWireVersion;
  h.header_len = kHeaderLenWords;
  h.type = static_cast<std::uint8_t>(PacketType::ACK);
  h.stream_id = cfg_.stream_id;
  h.payload_len = static_cast<std::uint16_t>(encoded_ack_size());
  encode_header(dg.bytes.data(), h);
  encode_ack(dg.bytes.data() + kHeaderSize, ack);
  counters_.acks_sent++;
  out.push_back(std::move(dg));
}

void UdpReceiver::on_timer(MonoNs now) {
  if (!synced_ || hole_last_nack_.empty()) return;
  // Re-NACK every still-open hole on the SRTT cadence (v0.2 §3.6). Without
  // this, a hole at the head of a burst (e.g., lost SYNC_START) never
  // regenerates a NACK and recovery falls to the RTO, missing the deadline.
  std::vector<std::uint32_t> due;
  for (const auto& [seq, last] : hole_last_nack_) {
    if (storage_.count(seq) != 0) continue;
    if (seq_covered_by_abandon(seq)) continue;
    if (now - last >= cfg_.nack_throttle_srtt && due.size() < kMaxNackCount) {
      due.push_back(seq);
    }
  }
  if (due.empty()) return;
  for (const std::uint32_t seq : due) hole_last_nack_[seq] = now;
  Nack nack;
  nack.session_epoch = cfg_.session_epoch;
  nack.seqs = std::move(due);
  Datagram dg;
  dg.peer = cfg_.peer;
  dg.bytes.assign(kHeaderSize + encoded_nack_size(nack.seqs.size()),
                  std::byte{0});
  Header h;
  h.version = kWireVersion;
  h.header_len = kHeaderLenWords;
  h.type = static_cast<std::uint8_t>(PacketType::NACK);
  h.stream_id = cfg_.stream_id;
  h.payload_len =
      static_cast<std::uint16_t>(encoded_nack_size(nack.seqs.size()));
  encode_header(dg.bytes.data(), h);
  encode_nack(dg.bytes.data() + kHeaderSize, nack);
  counters_.nacks_sent++;
  pending_control_.push_back(std::move(dg));
}

std::vector<Datagram> UdpReceiver::take_pending_control() {
  auto out = std::move(pending_control_);
  pending_control_.clear();
  return out;
}

void UdpReceiver::enqueue_hole_nacks(std::uint32_t upto_seq_exclusive,
                                     MonoNs now, std::vector<Datagram>& out) {
  Nack nack;
  nack.session_epoch = cfg_.session_epoch;
  for (std::uint32_t seq = recv_next_; seq_lt(seq, upto_seq_exclusive); ++seq) {
    if (storage_.count(seq) != 0) continue;
    if (seq_covered_by_abandon(seq)) continue;
    const auto it = hole_last_nack_.find(seq);
    if (it != hole_last_nack_.end() &&
        now - it->second < cfg_.nack_throttle_srtt) {
      continue;  // per-hole SRTT throttle
    }
    if (nack.seqs.size() >= kMaxNackCount) break;
    hole_last_nack_[seq] = now;
    nack.seqs.push_back(seq);
  }
  if (nack.seqs.empty()) return;
  Datagram dg;
  dg.peer = cfg_.peer;
  dg.bytes.assign(kHeaderSize + encoded_nack_size(nack.seqs.size()),
                  std::byte{0});
  Header h;
  h.version = kWireVersion;
  h.header_len = kHeaderLenWords;
  h.type = static_cast<std::uint8_t>(PacketType::NACK);
  h.stream_id = cfg_.stream_id;
  h.payload_len =
      static_cast<std::uint16_t>(encoded_nack_size(nack.seqs.size()));
  encode_header(dg.bytes.data(), h);
  encode_nack(dg.bytes.data() + kHeaderSize, nack);
  counters_.nacks_sent++;
  out.push_back(std::move(dg));
}

// ---------------------------------------------------------------------------
// ABANDON application (spec §4.5)
// ---------------------------------------------------------------------------

AbandonResult UdpReceiver::apply_abandon(std::vector<AbandonRange> ranges,
                                         MonoNs now) {
  AbandonResult result;
  ranges = normalize_abandon_ranges(std::move(ranges));
  ControlOutput out;  // reuse advance machinery

  for (const auto& r : ranges) {
    // Validate against the receive window (spec §4.5).
    if (!seq_lt(r.start, recv_next_ + static_cast<std::uint32_t>(cfg_.window_w))) {
      result.out_of_window_rejected = true;
      counters_.protocol_violations++;  // out-of-window ABANDON (v0.2 §3.7)
      continue;
    }
    if (seq_lt(r.end, recv_next_)) {
      // Entirely below recv_next: already consumed; record nothing (the
      // fresh ACK below still confirms the sender's stub cleanup).
      result.applied = true;
      continue;
    }

    AbandonRange clamped = r;
    if (seq_lt(clamped.start, recv_next_)) clamped.start = recv_next_;
    abandon_ranges_.push_back(clamped);
    result.applied_ranges.push_back(clamped);
    result.applied = true;

    // Flush covered storage entries; frames with held packets die.
    std::map<std::uint32_t, std::uint8_t> frames_hit;
    for (auto it = storage_.begin(); it != storage_.end();) {
      if (seq_ge(it->first, clamped.start) && seq_le(it->first, clamped.end)) {
        frames_hit[it->second.frame_id] = 1;
        ++result.flushed_packets;
        it = storage_.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& [fid, unused] : frames_hit) {
      (void)unused;
      mark_frame_dead(fid);
      result.dead_frames.push_back(fid);
    }
  }

  // A valid ABANDON always elicits a fresh ACK (spec §4.5). This is the
  // liveness path: if the ACK that followed the first application was lost,
  // the sender's idempotent ABANDON retransmission re-elicits it.
  if (result.applied) {
    const bool moved = advance_recv_next(now, out);
    if (!moved) enqueue_ack(out.datagrams);
    result.acks = std::move(out.datagrams);
    prune_hole_state();
  }
  return result;
}

}  // namespace rudp_rt
