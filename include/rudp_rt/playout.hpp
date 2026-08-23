#pragma once
// rudp_rt/playout.hpp — PlayoutPipeline: the client receive path as one deep
// module (spec §4.5/§4.7). It owns the three protocol components and
// everything they need to cooperate:
//
//   UdpReceiver      — sequence/window state, ACK/NACK, ABANDON application
//     -> FrameAssembler — frame-level assembly (INV-05/INV-08)
//       -> JitterBuffer   — playout scheduling (ONTIME/LATE, INV-09)
//
// Caller-visible behavior that used to be orchestrated by hand in the
// session core (and mirrored in test harnesses):
//   - descriptor-hook wiring between receiver and assembler;
//   - ABANDON dead-frame fan-out: every covered frame is marked dead in all
//     three components and counted exactly once (INV-06);
//   - playout expiry forwarding (ADR-0006): EXPIRED frames become jitter
//     tombstones, never deliveries;
//   - the bounded delivery consumer queue (ADR-0007, drop-newest);
//   - playout accounting (INV-21) and ADR-0012 latency observation.
//
// Stream/epoch validation and wire routing stay with the session core: the
// pipeline is handed an already-routed DATA datagram or decoded ABANDON
// ranges.

#include "rudp_rt/clock.hpp"
#include "rudp_rt/receiver.hpp"
#include "rudp_rt/wire.hpp"

#include <deque>
#include <optional>
#include <vector>

namespace rudp_rt {

class PlayoutPipeline {
 public:
  // assembler_frames bounds the frame table (INV-12); jitter_frames the
  // playout buffer; consumer_frames the delivery consumer queue (ADR-0007).
  PlayoutPipeline(ReceiverConfig rcfg, int assembler_frames, int jitter_frames,
                  MonoNs playout_delay, int consumer_frames, IClock& clock);

  // DATA path: classify / store / assemble / deliver. Returns control
  // datagrams (cumulative ACKs, hole NACKs) for the caller to route.
  std::vector<Datagram> on_data(const Datagram& dg, MonoNs now);

  // ABANDON path: apply ranges in the receiver, then fan frame death out to
  // the assembler and the jitter buffer (INV-06: one count per covered
  // frame). Returns the fresh ACK datagrams.
  std::vector<Datagram> apply_abandon(std::vector<AbandonRange> ranges,
                                      MonoNs now);

  // NACK re-emission, playout expiry scan (ADR-0006), and due deliveries
  // into the consumer queue. Returns control datagrams.
  std::vector<Datagram> on_timer(MonoNs now);

  // Session lifecycle (single-writer).
  void set_sync(std::uint32_t server_isn, std::uint32_t session_epoch);
  void set_nack_throttle(MonoNs srtt);
  void teardown();  // drops receive state and the timeline anchor

  // Next assembler-expiry or jitter-delivery deadline, if any.
  std::optional<MonoNs> next_deadline() const;

  // --- delivery output ---
  // Drains the bounded consumer queue; overflow was already counted as
  // consumer_dropped at push time.
  std::vector<JitterBuffer::Delivery> take_deliveries();

  // --- observability (INV-21 client side) ---
  std::uint64_t delivered_ontime() const { return delivered_ontime_; }
  std::uint64_t delivered_late() const { return delivered_late_; }
  std::uint64_t delivery_expired() const { return delivery_expired_; }
  std::uint64_t delivery_abandoned() const { return delivery_abandoned_; }
  std::uint64_t playout_dropped() const { return jitter_.dropped(); }
  std::uint64_t consumer_dropped() const { return consumer_dropped_; }
  void record_latencies(bool enable) { record_latencies_ = enable; }
  const std::vector<MonoNs>& latencies() const { return latencies_; }
  const std::vector<std::uint32_t>& delivered_frame_ids() const {
    return delivered_frame_ids_;
  }
  std::size_t jitter_depth() const { return jitter_.size(); }
  std::size_t reorder_depth() const { return receiver_.storage_size(); }
  const PacketCounters& receiver_counters() const {
    return receiver_.counters();
  }
  std::uint32_t recv_next() const { return receiver_.recv_next(); }

 private:
  void process_accepted(std::vector<AcceptedPacket>& accepted, MonoNs now);
  void observe_delivery(JitterBuffer::Delivery& d, MonoNs now);
  MonoNs capture_estimate(std::uint32_t rtp_ts) const;

  int consumer_frames_;
  UdpReceiver receiver_;
  FrameAssembler assembler_;
  JitterBuffer jitter_;
  MonoNs playout_delay_;

  // The RTP timeline anchor (spec §4.4): set once from the first valid
  // packet after sync; drives playout classification and latency estimates.
  bool anchor_set_ = false;
  MonoNs anchor_arrival_ = 0;
  std::uint32_t anchor_rtp_ts_ = 0;

  // Playout accounting (INV-21) and ADR-0012 observation.
  std::uint64_t delivered_ontime_ = 0;
  std::uint64_t delivered_late_ = 0;
  std::uint64_t delivery_expired_ = 0;
  std::uint64_t delivery_abandoned_ = 0;
  bool record_latencies_ = false;
  std::vector<MonoNs> latencies_;
  std::vector<std::uint32_t> delivered_frame_ids_;

  // Bounded consumer queue: deliveries wait here until take_deliveries().
  std::deque<JitterBuffer::Delivery> pending_deliveries_;
  std::uint64_t consumer_dropped_ = 0;
};

}  // namespace rudp_rt
