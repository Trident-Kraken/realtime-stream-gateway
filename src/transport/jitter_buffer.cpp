// jitter_buffer.cpp — static 64-frame playout buffer (spec §4.7).

#include "rudp_rt/media.hpp"
#include "rudp_rt/receiver.hpp"

#include <algorithm>
#include <cstdint>

namespace rudp_rt {

void JitterBuffer::set_anchor(MonoNs arrival, std::uint32_t rtp_timestamp,
                              MonoNs playout_delay) {
  anchored_ = true;
  anchor_arrival_ = arrival;
  anchor_rtp_ts_ = rtp_timestamp;
  playout_delay_ = playout_delay;
}

MonoNs JitterBuffer::playout_for(std::uint32_t rtp_timestamp) const {
  if (!anchored_) return INT64_MAX;
  return rtp_to_mono(anchor_arrival_, anchor_rtp_ts_, rtp_timestamp) +
         playout_delay_;
}

// Insert keeping `pending_` sorted by serial frame order so the front is the
// oldest not-yet-delivered frame (INV-09 ordering, overflow policy).
static std::size_t serial_rank(std::uint32_t frame_id, std::uint32_t base) {
  return frame_id - base;
}

std::vector<JitterBuffer::Delivery> JitterBuffer::push(CompletedFrame&& frame,
                                                       MonoNs now) {
  std::vector<Delivery> immediate;
  if (dead_.count(frame.frame_id) != 0) {
    dropped_++;  // dead frame never delivered (INV-08)
    return immediate;
  }
  const MonoNs playout = playout_for(frame.rtp_timestamp);
  if (anchored_ && playout + kNsPerMs <= now) {
    // Completion after playout (beyond the G tolerance): deliver
    // immediately as LATE.
    immediate.push_back(Delivery{std::move(frame), false});
    return immediate;
  }

  const std::uint32_t base =
      pending_.empty() ? frame.frame_id : pending_.front().frame.frame_id;
  Entry entry{std::move(frame), playout};
  const std::size_t rank = serial_rank(entry.frame.frame_id, base);
  auto pos = pending_.begin();
  while (pos != pending_.end() &&
         serial_rank(pos->frame.frame_id, base) < rank) {
    ++pos;
  }
  pending_.insert(pos, std::move(entry));

  if (static_cast<int>(pending_.size()) > capacity_) {
    // Drop the oldest not-yet-delivered frame (spec §3.3).
    pending_.erase(pending_.begin());
    dropped_++;
  }
  return immediate;
}

bool JitterBuffer::push_dead(std::uint32_t frame_id) {
  if (dead_.size() >= 256) dead_.erase(dead_.begin());
  dead_[frame_id] = 1;
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->frame.frame_id == frame_id) {
      pending_.erase(it);
      dropped_++;
      return true;
    }
  }
  return false;
}

std::vector<JitterBuffer::Delivery> JitterBuffer::on_timer(MonoNs now) {
  // Collect every due entry. pending_ is ordered by frame id; playout times
  // are not necessarily monotone across that order, so the scan must not
  // stop at the first non-due front entry (liveness, INV-09 keeps the
  // frame-id order of the delivered set).
  //
  // On-time classification tolerates one scheduling granularity G (the same
  // 1 ms constant the RTO uses): a real clock fires the delivery scan a
  // wake-latency epsilon after the playout instant, which must not flip a
  // punctual delivery to "late". Under the virtual clock the scan lands
  // exactly on the playout and the tolerance is inactive.
  std::vector<Delivery> deliveries;
  std::vector<std::size_t> due_idx;
  for (std::size_t i = 0; i < pending_.size(); ++i) {
    if (pending_[i].playout <= now) due_idx.push_back(i);
  }
  for (const std::size_t i : due_idx) {
    Entry entry = std::move(pending_[i]);
    const bool ontime = now <= entry.playout + kNsPerMs;
    deliveries.push_back(Delivery{std::move(entry.frame), ontime});
  }
  if (!due_idx.empty()) {
    std::vector<std::size_t> keep;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
      if (pending_[i].playout > now) keep.push_back(i);
    }
    std::vector<Entry> remaining;
    remaining.reserve(keep.size());
    for (const std::size_t i : keep) remaining.push_back(std::move(pending_[i]));
    pending_ = std::move(remaining);
  }
  // Prune dead tombstones that can no longer matter.
  if (!pending_.empty() && !dead_.empty()) {
    const std::uint32_t oldest = pending_.front().frame.frame_id;
    for (auto it = dead_.begin(); it != dead_.end() && frame_lt(it->first, oldest);) {
      it = dead_.erase(it);
    }
  }
  return deliveries;
}

std::optional<MonoNs> JitterBuffer::next_delivery_deadline() const {
  if (pending_.empty()) return std::nullopt;
  std::optional<MonoNs> best;
  for (const auto& e : pending_) {
    if (!best || e.playout < *best) best = e.playout;
  }
  return best;
}

}  // namespace rudp_rt
