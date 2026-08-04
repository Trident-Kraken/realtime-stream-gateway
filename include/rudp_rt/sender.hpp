#pragma once
// rudp_rt/sender.hpp — TxFrame/TxPacket state machine (spec §4.4–§4.6).
//
// Single-writer: every method must be called from the owning Reactor/client
// thread only. Outputs are value-owned datagrams and frame-closure events
// drained via take_output(). The public method set matches the minimum
// contract in spec §5; IClock is injected because on_frame() carries no
// explicit timestamp.

#include "rudp_rt/clock.hpp"
#include "rudp_rt/media.hpp"
#include "rudp_rt/metrics.hpp"
#include "rudp_rt/types.hpp"
#include "rudp_rt/wire.hpp"

#include <deque>
#include <optional>
#include <vector>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Packetizer output: one frame split into RTP+fragment packets.
// ---------------------------------------------------------------------------
struct PacketizedFragment {
  std::uint16_t index = 0;
  std::uint16_t rtp_seq = 0;
  std::uint32_t offset = 0;  // into PacketizedFrame::bytes
  std::uint32_t len = 0;
  bool frame_end = false;
};

struct PacketizedFrame {
  std::uint16_t stream_id = 0;
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint32_t ssrc = 0;
  MonoNs capture_time = 0;
  MonoNs deadline_budget = 0;
  FrameType frame_type = FrameType::KEY_FRAME;
  std::uint32_t depends_on_frame = 0;
  std::vector<std::byte> bytes;  // full media (owned)
  std::vector<PacketizedFragment> fragments;

  std::uint16_t frame_count() const {
    return static_cast<std::uint16_t>(fragments.size());
  }
};

// Splits a MediaFrame into fragments of at most media_payload_bytes.
class Packetizer {
 public:
  // Returns nullopt when the frame needs more than max_frame_packets packets.
  std::optional<PacketizedFrame> packetize(MediaFrame&& frame,
                                           std::uint32_t ssrc,
                                           int media_payload_bytes,
                                           int max_frame_packets);

  std::uint16_t next_rtp_seq() { return rtp_seq_++; }

 private:
  std::uint16_t rtp_seq_ = 1;
};

// ---------------------------------------------------------------------------
// RFC 6298-style RTO estimation (ADR-0011). Pure value type.
// ---------------------------------------------------------------------------
class RtoEstimator {
 public:
  void configure(MonoNs srtt_initial, MonoNs rttvar_initial, MonoNs rto_min,
                 MonoNs rto_max);
  void sample(MonoNs r);
  MonoNs srtt() const { return srtt_; }
  MonoNs rttvar() const { return rttvar_; }
  MonoNs current() const;  // SRTT + max(G, 4*RTTVAR), clamped
  MonoNs admission_threshold() const { return srtt_ + 4 * rttvar_; }

 private:
  MonoNs srtt_ = 0;
  MonoNs rttvar_ = 0;
  MonoNs rto_min_ = kNsPerMs;
  MonoNs rto_max_ = kNsPerSec;
  bool has_sample_ = false;
};

// ---------------------------------------------------------------------------
// Sender state
// ---------------------------------------------------------------------------

struct TxPacket {
  std::uint32_t seq = 0;
  std::uint16_t frame_index = 0;
  std::uint16_t rtp_seq = 0;
  std::uint32_t offset = 0;  // into owning TxFrame::media
  std::uint32_t len = 0;
  bool frame_end = false;
  MonoNs first_send = -1;   // -1 = never sent
  MonoNs last_send = -1;
  std::uint16_t retransmissions = 0;
  bool in_flight = false;
  bool sacked = false;
};

struct TxFrame {
  std::uint32_t frame_id = 0;
  std::uint32_t rtp_timestamp = 0;
  std::uint16_t stream_id = 0;
  std::uint32_t ssrc = 0;
  std::uint32_t first_seq = 0;
  std::uint16_t frame_count = 0;
  MonoNs deadline_abs = 0;
  FrameType frame_type = FrameType::KEY_FRAME;
  std::uint32_t depends_on_frame = 0;
  std::vector<std::byte> media;   // payload owned here until terminal (§3.4)
  std::vector<TxPacket> packets;
  bool abandoned = false;
  bool sync_start = false;
  bool closed = false;            // ledger closure already emitted
  MonoNs last_abandon_send = -1;
  std::uint16_t abandon_retries = 0;
};

struct FrameClosure {
  std::uint32_t frame_id = 0;
  SessionFrameOutcome outcome = SessionFrameOutcome::ABANDONED;
};

struct AdmissionNotice {
  std::uint32_t frame_id = 0;
  bool admitted = false;  // false => PRE_ADMISSION_DROPPED
};

struct SenderOutput {
  std::vector<Datagram> data;        // new DATA + retransmitted DATA
  std::vector<Datagram> control;     // ABANDON datagrams
  std::vector<FrameClosure> closures;
  std::vector<AdmissionNotice> admissions;
  bool keyframe_requested = false;
};

struct SenderConfig {
  Endpoint peer;
  std::uint16_t stream_id = 1;
  std::uint32_t ssrc = 0;
  std::uint32_t session_epoch = 0;
  std::uint32_t isn = 0;
  int window_w = 64;
  int max_frame_packets = 10;
  int media_payload_bytes = 1200;
  MonoNs rto_min = ns_from_ms(50);
  MonoNs rto_max = ns_from_ms(1000);
  MonoNs srtt_initial = ns_from_ms(25);
  MonoNs rttvar_initial = ns_from_ms(25) / 2;
  bool deadline_mode = true;  // false = always-retransmit arm (ADR-0012)
};

// ---------------------------------------------------------------------------
// Abstract contract (spec §5). Sender is the interface; TransportSender the
// single implementation.
// ---------------------------------------------------------------------------
class Sender {
 public:
  virtual ~Sender() = default;
  virtual void on_frame(MediaFrame frame) = 0;
  virtual void on_ack(const Ack& ack, MonoNs now) = 0;
  virtual void on_nack(const Nack& nack, MonoNs now) = 0;
  virtual void on_timer(MonoNs now) = 0;

  // Emission support (single-writer; called by the session pump).
  // Emits at most one new (never-sent) DATA packet per call while budget
  // remains; marks it sent (first_send/last_send/in_flight).
  virtual std::optional<Datagram> emit_new_data(MonoNs now,
                                                std::int64_t byte_budget) = 0;
  virtual SenderOutput take_output() = 0;
  virtual std::optional<MonoNs> next_deadline() const = 0;
  virtual const PacketCounters& counters() const = 0;
  virtual const RtoEstimator& rtt() const = 0;
  virtual std::uint64_t media_bytes_owned() const = 0;
  virtual std::uint32_t allocated_unacked() const = 0;
  virtual bool window_empty() const = 0;
  // Finalizes every outstanding frame as ABANDONED with the given cause.
  virtual void teardown(TeardownCause cause, MonoNs now) = 0;
};

// ---------------------------------------------------------------------------
// Concrete sender.
// ---------------------------------------------------------------------------
class TransportSender final : public Sender {
 public:
  TransportSender() = default;  // for move-assign into session members
  TransportSender(SenderConfig cfg, IClock& clock);

  void on_frame(MediaFrame frame) override;
  void on_ack(const Ack& ack, MonoNs now) override;
  void on_nack(const Nack& nack, MonoNs now) override;
  void on_timer(MonoNs now) override;

  std::optional<Datagram> emit_new_data(MonoNs now,
                                        std::int64_t byte_budget) override;
  SenderOutput take_output() override { return std::move(out_); }
  std::optional<MonoNs> next_deadline() const override;
  const PacketCounters& counters() const override { return counters_; }
  const RtoEstimator& rtt() const override { return rtt_; }
  std::uint64_t media_bytes_owned() const override { return media_bytes_; }
  std::uint32_t allocated_unacked() const override {
    return static_cast<std::uint32_t>(seq_forward(send_base_, next_seq_));
  }
  bool window_empty() const override { return frames_.empty(); }
  void teardown(TeardownCause cause, MonoNs now) override;

  // Test/observability hooks.

 private:
  struct FrameSlot {
    TxFrame frame;
  };

  TxFrame* find_frame_for_seq(std::uint32_t seq);
  MonoNs effective_rto(std::uint16_t retries) const;
  Datagram build_data(const TxFrame& f, const TxPacket& p, bool retransmit) const;
  void abandon_frame(TxFrame& f, MonoNs now);
  void close_frame(TxFrame& f, SessionFrameOutcome outcome, MonoNs now);
  void retransmit_packet(TxFrame& f, TxPacket& p, MonoNs now);
  void queue_abandon_send(TxFrame& f, MonoNs now);
  void emit_abandon_ranges(std::vector<AbandonRange> ranges);
  void apply_sack_bits(const Ack& ack);
  void pop_covered_frames(MonoNs now);
  void release_frame_media(TxFrame& f);

  void cascade_abandon_dependents(std::uint32_t root_abandoned_frame_id,
                                  MonoNs now,
                                  std::vector<AbandonRange>& pending_ranges);
  void record_abandoned_frame(std::uint32_t fid);
  bool is_dependency_abandoned(std::uint32_t fid) const;

  SenderConfig cfg_;
  IClock* clock_;
  Packetizer packetizer_;
  RtoEstimator rtt_;

  std::deque<TxFrame> frames_;       // allocation order == seq order
  std::uint32_t send_base_ = 0;      // lowest un-cumulatively-ACKed seq
  std::uint32_t next_seq_ = 0;       // next seq to allocate
  std::uint32_t last_valid_ack_ = 0; // last cumulatively consumed seen
  bool sync_start_pending_ = true;   // first admitted frame carries SYNC_START
  bool any_frame_admitted_ = false;
  bool torn_down_ = false;

  PacketCounters counters_;
  SenderOutput out_;
  std::uint64_t media_bytes_ = 0;
  std::deque<std::uint32_t> abandoned_frame_ids_;
};

}  // namespace rudp_rt
