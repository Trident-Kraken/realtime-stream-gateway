// frame_assembler.cpp — frame-level assembly over accepted packets
// (spec §4.7; INV-05/INV-06/INV-08).

#include "rudp_rt/media.hpp"
#include "rudp_rt/receiver.hpp"

#include <algorithm>
#include <cstdint>

namespace rudp_rt {

void FrameAssembler::set_anchor(MonoNs arrival, std::uint32_t rtp_timestamp,
                                MonoNs playout_delay) {
  anchored_ = true;
  anchor_arrival_ = arrival;
  anchor_rtp_ts_ = rtp_timestamp;
  playout_delay_ = playout_delay;
}

MonoNs FrameAssembler::playout_for(std::uint32_t rtp_timestamp) const {
  if (!anchored_) return INT64_MAX;
  return rtp_to_mono(anchor_arrival_, anchor_rtp_ts_, rtp_timestamp) +
         playout_delay_;
}

std::uint32_t FrameAssembler::key_of(std::uint32_t frame_id) const {
  return frame_id - frame_key_base_;
}

FrameAssembler::RxFrame* FrameAssembler::find_frame(std::uint32_t frame_id) {
  const auto it = frames_.find(key_of(frame_id));
  return it == frames_.end() ? nullptr : &it->second;
}

DataClassification FrameAssembler::validate_descriptor(
    const AcceptedPacket& packet) {
  if (!frame_key_base_valid_) return DataClassification::VALID;  // new frame
  RxFrame* f = find_frame(packet.frame_id);
  if (f == nullptr) return DataClassification::VALID;  // unknown: create later
  if (f->dead) return DataClassification::DESCRIPTOR_CONFLICT;
  const std::uint32_t first_seq = packet.seq - packet.frame_index;
  if (f->frame_count != packet.frame_count ||
      f->rtp_timestamp != packet.rtp_timestamp ||
      f->first_seq != first_seq) {
    return DataClassification::DESCRIPTOR_CONFLICT;  // INV-05
  }
  return DataClassification::VALID;
}

AssemblerOutput FrameAssembler::on_packet(AcceptedPacket&& packet) {
  AssemblerOutput out;
  RxFrame* f = find_frame(packet.frame_id);
  if (f == nullptr) {
    if (frames_.size() >= static_cast<std::size_t>(max_tracked_frames_)) {
      // Bounded frame table: expire the oldest incomplete frames (INV-12).
      while (frames_.size() >= static_cast<std::size_t>(max_tracked_frames_)) {
        auto oldest = frames_.begin();
        out.expired_frames.push_back(oldest->second.frame_id);
        frames_.erase(oldest);
      }
      frame_key_base_valid_ = false;
    }
    if (!frame_key_base_valid_) {
      frame_key_base_ = packet.frame_id;
      frame_key_base_valid_ = true;
    }
    RxFrame nf;
    nf.frame_id = packet.frame_id;
    nf.rtp_timestamp = packet.rtp_timestamp;
    nf.first_seq = packet.seq - packet.frame_index;
    nf.frame_count = packet.frame_count;
    nf.stream_id = packet.stream_id;
    nf.fragments.resize(nf.frame_count);
    const auto key = key_of(packet.frame_id);
    auto [it, inserted] = frames_.emplace(key, std::move(nf));
    (void)inserted;
    f = &it->second;
  }
  if (f->dead) return out;  // dead frames never assemble (INV-08)

  const std::uint32_t bit = 1u << packet.frame_index;
  if (f->received_mask & bit) return out;  // defensive duplicate

  f->received_mask |= bit;
  f->received_count++;
  if (packet.sync_start) f->sync_start_seen = true;
  f->first_arrival = std::min<MonoNs>(f->first_arrival, packet.arrival);
  f->last_arrival = std::max<MonoNs>(f->last_arrival, packet.arrival);
  f->fragments[packet.frame_index] = std::move(packet.media);

  // Completion check (INV-08): all required indexes present.
  if (f->received_count == f->frame_count) {
    out.completed.push_back(complete_frame(*f));
    frames_.erase(key_of(f->frame_id));
    if (frames_.empty()) frame_key_base_valid_ = false;
  }
  return out;
}

CompletedFrame FrameAssembler::complete_frame(RxFrame& f) {
  CompletedFrame cf;
  cf.stream_id = f.stream_id;
  cf.frame_id = f.frame_id;
  cf.rtp_timestamp = f.rtp_timestamp;
  cf.frame_count = f.frame_count;
  cf.first_arrival = f.first_arrival;
  cf.last_arrival = f.last_arrival;
  std::size_t total = 0;
  for (const auto& frag : f.fragments) total += frag.size();
  cf.bytes.reserve(total);
  for (auto& frag : f.fragments) {
    cf.bytes.insert(cf.bytes.end(), frag.begin(), frag.end());
  }
  return cf;
}

AssemblerOutput FrameAssembler::on_timer(MonoNs now) {
  AssemblerOutput out;
  for (auto it = frames_.begin(); it != frames_.end();) {
    RxFrame& f = it->second;
    if (f.dead || f.received_count == f.frame_count) {
      ++it;
      continue;
    }
    if (playout_for(f.rtp_timestamp) <= now) {
      out.expired_frames.push_back(f.frame_id);  // EXPIRED (ADR-0006)
      it = frames_.erase(it);
      if (frames_.empty()) frame_key_base_valid_ = false;
    } else {
      ++it;
    }
  }
  return out;
}

void FrameAssembler::on_abandon_ranges(const std::vector<AbandonRange>& ranges,
                                       std::vector<std::uint32_t>& dead_out) {
  for (auto it = frames_.begin(); it != frames_.end();) {
    RxFrame& f = it->second;
    const std::uint32_t last_seq =
        seq_advance(f.first_seq, f.frame_count ? f.frame_count - 1 : 0);
    bool covered = false;
    for (const auto& r : ranges) {
      if (seq_le(r.start, last_seq) && seq_ge(r.end, f.first_seq)) {
        covered = true;
        break;
      }
    }
    if (covered) {
      dead_out.push_back(f.frame_id);
      it = frames_.erase(it);
      if (frames_.empty()) frame_key_base_valid_ = false;
    } else {
      ++it;
    }
  }
}

void FrameAssembler::on_frame_dead(std::uint32_t frame_id) {
  if (!frame_key_base_valid_) return;
  frames_.erase(key_of(frame_id));
  if (frames_.empty()) frame_key_base_valid_ = false;
}

std::optional<MonoNs> FrameAssembler::next_expiry_deadline() const {
  std::optional<MonoNs> best;
  for (const auto& [key, f] : frames_) {
    (void)key;
    if (f.dead || f.received_count == f.frame_count) continue;
    const MonoNs playout = playout_for(f.rtp_timestamp);
    if (!best || playout < *best) best = playout;
  }
  return best;
}

}  // namespace rudp_rt
