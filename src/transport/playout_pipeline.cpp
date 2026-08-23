// playout_pipeline.cpp — PlayoutPipeline implementation: the cooperation
// logic between receiver, assembler, and jitter buffer lives here, so the
// session core and test harnesses drive one interface instead of five
// hand-wired steps.

#include "rudp_rt/playout.hpp"

#include <algorithm>

namespace rudp_rt {

PlayoutPipeline::PlayoutPipeline(ReceiverConfig rcfg, int assembler_frames,
                                 int jitter_frames, MonoNs playout_delay,
                                 int consumer_frames, IClock& clock)
    : consumer_frames_(consumer_frames),
      receiver_(std::move(rcfg), clock),
      assembler_(assembler_frames),
      jitter_(jitter_frames),
      playout_delay_(playout_delay) {
  receiver_.set_descriptor_hooks(
      [this](const AcceptedPacket& p) {
        return assembler_.validate_descriptor(p);
      },
      [this](std::uint32_t fid) { assembler_.on_frame_dead(fid); });
}

std::vector<Datagram> PlayoutPipeline::on_data(const Datagram& dg,
                                               MonoNs now) {
  ControlOutput out = receiver_.on_datagram(dg, now);
  process_accepted(out.accepted, now);
  return std::move(out.datagrams);
}

std::vector<Datagram> PlayoutPipeline::apply_abandon(
    std::vector<AbandonRange> ranges, MonoNs now) {
  AbandonResult result = receiver_.apply_abandon(std::move(ranges), now);

  // Mark covered frames dead across the pipeline (spec §4.5). A frame that
  // shows up in more than one dead list is still counted exactly once
  // (INV-06 terminal uniqueness).
  std::vector<std::uint32_t> assembler_dead;
  assembler_.on_abandon_ranges(result.applied_ranges, assembler_dead);
  std::vector<std::uint32_t> all_dead = result.dead_frames;
  all_dead.insert(all_dead.end(), assembler_dead.begin(), assembler_dead.end());
  std::sort(all_dead.begin(), all_dead.end());
  all_dead.erase(std::unique(all_dead.begin(), all_dead.end()), all_dead.end());
  for (const std::uint32_t id : all_dead) {
    const bool jitter_had = jitter_.push_dead(id);
    receiver_.mark_frame_dead(id);
    const bool in_assembler =
        std::find(assembler_dead.begin(), assembler_dead.end(), id) !=
        assembler_dead.end();
    const bool in_receiver =
        std::find(result.dead_frames.begin(), result.dead_frames.end(), id) !=
        result.dead_frames.end();
    if (jitter_had || in_assembler || in_receiver) {
      delivery_abandoned_++;
    }
  }
  return std::move(result.acks);
}

std::vector<Datagram> PlayoutPipeline::on_timer(MonoNs now) {
  std::vector<Datagram> control = receiver_.on_timer_and_take(now);
  AssemblerOutput aout = assembler_.on_timer(now);
  for (const std::uint32_t expired_fid : aout.expired_frames) {
    delivery_expired_++;  // EXPIRED at playout scan (ADR-0006)
    // Delivery-level death only: the frame is never delivered, but its
    // packets keep filling sequence holes so the window can drain
    // (spec §4.5/§4.7; the always-retransmit arm never sends ABANDON).
    jitter_.push_dead(expired_fid);
  }
  for (auto& delivery : jitter_.on_timer(now)) {
    observe_delivery(delivery, now);
  }
  return control;
}

void PlayoutPipeline::set_sync(std::uint32_t server_isn,
                               std::uint32_t session_epoch) {
  receiver_.set_sync(server_isn, session_epoch);
}

void PlayoutPipeline::set_nack_throttle(MonoNs srtt) {
  receiver_.set_nack_throttle(srtt);
}

void PlayoutPipeline::teardown() {
  receiver_.teardown();
  anchor_set_ = false;
}

std::optional<MonoNs> PlayoutPipeline::next_deadline() const {
  std::optional<MonoNs> best;
  if (auto t = assembler_.next_expiry_deadline()) best = *t;
  if (auto t = jitter_.next_delivery_deadline()) {
    if (!best || *t < *best) best = *t;
  }
  return best;
}

std::vector<JitterBuffer::Delivery> PlayoutPipeline::take_deliveries() {
  std::vector<JitterBuffer::Delivery> out(
      std::make_move_iterator(pending_deliveries_.begin()),
      std::make_move_iterator(pending_deliveries_.end()));
  pending_deliveries_.clear();
  return out;
}

void PlayoutPipeline::process_accepted(std::vector<AcceptedPacket>& accepted,
                                       MonoNs now) {
  for (auto& ap : accepted) {
    if (!anchor_set_) {
      // First valid packet with a consistent descriptor anchors the RTP
      // timeline (spec §4.4/§4.7).
      assembler_.set_anchor(ap.arrival, ap.rtp_timestamp, playout_delay_);
      jitter_.set_anchor(ap.arrival, ap.rtp_timestamp, playout_delay_);
      anchor_arrival_ = ap.arrival;
      anchor_rtp_ts_ = ap.rtp_timestamp;
      anchor_set_ = true;
    }
    AssemblerOutput aout = assembler_.on_packet(std::move(ap));
    for (const std::uint32_t dead : aout.dead_frames) {
      receiver_.mark_frame_dead(dead);
    }
    for (const std::uint32_t overflow_fid : aout.expired_frames) {
      (void)overflow_fid;
      delivery_expired_++;  // frame-table overflow expiry (INV-12)
    }
    for (auto& cf : aout.completed) {
      for (auto& delivery : jitter_.push(std::move(cf), now)) {
        observe_delivery(delivery, now);
      }
    }
  }
}

MonoNs PlayoutPipeline::capture_estimate(std::uint32_t rtp_ts) const {
  return rtp_to_mono(anchor_arrival_, anchor_rtp_ts_, rtp_ts);
}

void PlayoutPipeline::observe_delivery(JitterBuffer::Delivery& d, MonoNs now) {
  if (d.ontime) {
    delivered_ontime_++;
  } else {
    delivered_late_++;
  }
  if (record_latencies_) {
    latencies_.push_back(now - capture_estimate(d.frame.rtp_timestamp));
    delivered_frame_ids_.push_back(d.frame.frame_id);
  }
  if (pending_deliveries_.size() >=
      static_cast<std::size_t>(consumer_frames_)) {
    consumer_dropped_++;  // drop newest (ADR-0007)
  } else {
    pending_deliveries_.push_back(std::move(d));
  }
}

}  // namespace rudp_rt
