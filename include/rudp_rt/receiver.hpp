#pragma once
// rudp_rt/receiver.hpp — receive window, reorder storage, and ACK/NACK state
// (spec §4.5, §4.7). Single-writer: Reactor/client thread only.
//
// The Receiver owns sequence-level state only: classification, reorder
// storage, hole NACKs, cumulative-ACK advancement, and ABANDON application.
// Frame-level assembly is FrameAssembler's job; accepted packets flow
// Receiver -> assembler through ControlOutput::accepted.

#include "rudp_rt/clock.hpp"
#include "rudp_rt/media.hpp"
#include "rudp_rt/metrics.hpp"
#include "rudp_rt/types.hpp"
#include "rudp_rt/wire.hpp"

#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace rudp_rt {

// RTP 90 kHz timestamp -> monotonic time, relative to the timeline anchor
// (first valid packet after sync, spec §4.4). Single definition of the
// mapping; the assembler, the jitter buffer, and playout latency estimation
// must all agree on it.
inline MonoNs rtp_to_mono(MonoNs anchor_arrival, std::uint32_t anchor_rtp_ts,
                          std::uint32_t rtp_timestamp) {
  const std::int32_t delta =
      static_cast<std::int32_t>(rtp_timestamp - anchor_rtp_ts);
  return anchor_arrival +
         static_cast<MonoNs>(
             (static_cast<std::int64_t>(delta) * kNsPerSec) / kRtpClockRate);
}

struct ReceiverConfig {
  Endpoint peer;                 // control datagrams are sent here
  std::uint16_t stream_id = 1;
  std::uint32_t session_epoch = 0;
  std::uint32_t isn = 0;         // server_isn from PONG; recv_next starts here
  int window_w = 64;             // receive window (D-20: both sides agree)
  int reorder_capacity = 256;
  MonoNs nack_throttle_srtt = ns_from_ms(25);
};

// Abandon-processing result (spec §4.5): ranges normalized, storage flushed,
// recv_next advanced over received or abandoned sequences, fresh ACK queued.
struct AbandonResult {
  bool applied = false;
  bool out_of_window_rejected = false;
  std::uint32_t flushed_packets = 0;
  std::vector<AbandonRange> applied_ranges;  // clamped, normalized
  std::vector<std::uint32_t> dead_frames;    // frames covered by the ranges
  std::vector<Datagram> acks;                // fresh ACK to send
};

// ---------------------------------------------------------------------------
// Abstract contract (spec §5); UdpReceiver is the single implementation.
// ---------------------------------------------------------------------------
class Receiver {
 public:
  virtual ~Receiver() = default;
  virtual ControlOutput on_datagram(const Datagram& datagram, MonoNs now) = 0;
  virtual void on_timer(MonoNs) = 0;
};

class UdpReceiver final : public Receiver {
 public:
  UdpReceiver(ReceiverConfig cfg, IClock& clock);

  ControlOutput on_datagram(const Datagram& datagram, MonoNs now) override;
  // Per-hole NACK re-emission: every hole keeps NACKing on the SRTT cadence
  // until it is filled (v0.2 §3.6); accumulated datagrams drain through
  // take_pending_control().
  void on_timer(MonoNs now) override;
  // on_timer + drain, for cores that route control directly.
  std::vector<Datagram> on_timer_and_take(MonoNs now) {
    on_timer(now);
    return take_pending_control();
  }
  std::vector<Datagram> take_pending_control();

  // Session lifecycle hooks (single-writer).
  void set_sync(std::uint32_t server_isn, std::uint32_t session_epoch);  // PONG OK
  void set_nack_throttle(MonoNs srtt);
  void teardown();  // drops all state; stale events stay harmless

  // ABANDON application (called by the client core after epoch validation).
  AbandonResult apply_abandon(std::vector<AbandonRange> ranges, MonoNs now);

  const PacketCounters& counters() const { return counters_; }
  bool synced() const { return synced_; }
  std::uint32_t recv_next() const { return recv_next_; }
  std::size_t storage_size() const { return storage_.size(); }

  // Tombstones: frames declared dead (conflict or ABANDON) so later packets
  // are dropped without new violations.
  bool frame_dead(std::uint32_t frame_id) const;
  void mark_frame_dead(std::uint32_t frame_id);

  // Descriptor bookkeeping for conflict pre-check lives in the assembler;
  // the receiver consults it via these callbacks to keep classification
  // order intact (spec §4.7).
  void set_descriptor_hooks(std::function<DataClassification(
                                const AcceptedPacket&)> validate,
                            std::function<void(std::uint32_t)> kill_frame);

 private:
  struct StoredPacket {
    std::uint32_t frame_id = 0;
    std::uint16_t frame_index = 0;
    MonoNs arrival = 0;
    bool reported = false;
    // Media bytes are moved into the AcceptedPacket at accept time; the
    // entry stays as held-state metadata until cumulative consumption.
  };

  void enqueue_ack(std::vector<Datagram>& out);
  void enqueue_hole_nacks(std::uint32_t upto_seq_exclusive, MonoNs now,
                          std::vector<Datagram>& out);
  bool advance_recv_next(MonoNs now, ControlOutput& out);
  bool seq_covered_by_abandon(std::uint32_t seq) const;
  void prune_hole_state();

  ReceiverConfig cfg_;
  IClock& clock_;
  bool synced_ = false;
  std::uint32_t recv_next_ = 0;
  std::map<std::uint32_t, StoredPacket> storage_;   // absolute seq key
  std::map<std::uint32_t, MonoNs> hole_last_nack_;  // per-hole throttle
  std::vector<Datagram> pending_control_;           // NACKs from on_timer
  std::vector<AbandonRange> abandon_ranges_;
  std::map<std::uint32_t, std::uint8_t> dead_frames_;  // tombstones
  std::uint64_t bytes_owned_ = 0;
  PacketCounters counters_;

  std::function<DataClassification(const AcceptedPacket&)> validate_descriptor_;
  std::function<void(std::uint32_t)> kill_frame_;
};

// ---------------------------------------------------------------------------
// FrameAssembler — frame-level state machine over accepted packets
// (spec §4.7; INV-05/INV-08). Descriptor fixed by first accepted packet;
// conflict kills the frame; completion emits a CompletedFrame exactly once.
// Expiry is lazy: frames still incomplete when their playout scan runs are
// EXPIRED (ADR-0006).
// ---------------------------------------------------------------------------
struct AssemblerOutput {
  std::vector<CompletedFrame> completed;
  std::vector<std::uint32_t> dead_frames;    // descriptor conflicts
  std::vector<std::uint32_t> expired_frames; // EXPIRED at playout scan
};

class FrameAssembler {
 public:
  explicit FrameAssembler(int max_tracked_frames = 128)
      : max_tracked_frames_(max_tracked_frames) {}

  // RTP timeline anchor: first valid packet after sync (spec §4.4).
  void set_anchor(MonoNs arrival, std::uint32_t rtp_timestamp,
                  MonoNs playout_delay);
  bool anchored() const { return anchored_; }

  AssemblerOutput on_packet(AcceptedPacket&& packet);
  AssemblerOutput on_timer(MonoNs now);  // expiry scan at/below playout
  void on_abandon_ranges(const std::vector<AbandonRange>& ranges,
                         std::vector<std::uint32_t>& dead_out);
  void on_frame_dead(std::uint32_t frame_id);

  // Pre-storage descriptor validation called by the receiver so the spec §4.7
  // classification order (… DUPLICATE, descriptor conflict, VALID) holds
  // before any storage mutation. Creates the descriptor when the frame is
  // new; returns DESCRIPTOR_CONFLICT when it contradicts a fixed descriptor.
  DataClassification validate_descriptor(const AcceptedPacket& packet);

  std::optional<MonoNs> next_expiry_deadline() const;
  std::size_t tracked_frames() const { return frames_.size(); }
  MonoNs playout_for(std::uint32_t rtp_timestamp) const;

 private:
  struct RxFrame {
    std::uint32_t frame_id = 0;
    std::uint32_t rtp_timestamp = 0;
    std::uint32_t first_seq = 0;   // derived: seq - index
    std::uint16_t frame_count = 0;
    std::uint32_t received_mask = 0;   // bit per packet (count <= 64)
    std::uint16_t received_count = 0;
    std::uint16_t stream_id = 0;
    bool sync_start_seen = false;
    bool dead = false;
    MonoNs first_arrival = INT64_MAX;
    MonoNs last_arrival = 0;
    std::vector<std::vector<std::byte>> fragments;  // per packet index
  };

  static constexpr std::uint32_t kKeyBase = 0;  // relative key = frame_id - base
  std::uint32_t key_of(std::uint32_t frame_id) const;

  RxFrame* find_frame(std::uint32_t frame_id);
  CompletedFrame complete_frame(RxFrame& f);

  int max_tracked_frames_;
  bool anchored_ = false;
  MonoNs anchor_arrival_ = 0;
  std::uint32_t anchor_rtp_ts_ = 0;
  MonoNs playout_delay_ = 0;
  std::map<std::uint32_t, RxFrame> frames_;  // key = serial offset from base
  std::uint32_t frame_key_base_ = 0;
  bool frame_key_base_valid_ = false;
};

// ---------------------------------------------------------------------------
// JitterBuffer — 64-frame static playout buffer (spec §4.7).
// Delivery at or before playout is ONTIME; after playout is LATE; overflow
// drops the oldest not-yet-delivered frame (playout_dropped); dead frames
// are never delivered.
// ---------------------------------------------------------------------------
class JitterBuffer {
 public:
  struct Delivery {
    CompletedFrame frame;
    bool ontime = true;
  };

  explicit JitterBuffer(int capacity = 64) : capacity_(capacity) {}

  void set_anchor(MonoNs arrival, std::uint32_t rtp_timestamp,
                  MonoNs playout_delay);
  // Returns frames delivered immediately (completion after playout = LATE).
  std::vector<Delivery> push(CompletedFrame&& frame, MonoNs now);
  // Returns true when a pending entry existed for the frame.
  bool push_dead(std::uint32_t frame_id);
  std::vector<Delivery> on_timer(MonoNs now);
  std::optional<MonoNs> next_delivery_deadline() const;
  std::size_t size() const { return pending_.size(); }
  std::uint64_t dropped() const { return dropped_; }

 private:
  struct Entry {
    CompletedFrame frame;
    MonoNs playout = 0;
  };
  MonoNs playout_for(std::uint32_t rtp_timestamp) const;

  int capacity_;
  bool anchored_ = false;
  MonoNs anchor_arrival_ = 0;
  std::uint32_t anchor_rtp_ts_ = 0;
  MonoNs playout_delay_ = 0;
  std::vector<Entry> pending_;  // kept sorted by serial frame order
  std::map<std::uint32_t, std::uint8_t> dead_;
  std::uint64_t dropped_ = 0;
};

}  // namespace rudp_rt
