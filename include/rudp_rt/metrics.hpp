#pragma once
// rudp_rt/metrics.hpp — the two ledgers and immutable snapshots (spec §6,
// ADR-0005). Frame-level ledgers and packet-level counters are strictly
// separated (INV-21). Both conservation equations are checked continuously:
//
//   source_frames   = capture_dropped + admitted_to_fanout
//   session_offered = ontime + late + abandoned + expired
//                   + pre_admission_dropped + in_flight
//
// A failed check is a release blocker and aborts the process (INV-20).

#include "rudp_rt/types.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace rudp_rt {

// ---------------------------------------------------------------------------
// Packet-level counters. These never alter frame-level counts (INV-21).
// ---------------------------------------------------------------------------
struct PacketCounters {
  std::uint64_t data_sent = 0;
  std::uint64_t data_retransmissions = 0;
  std::uint64_t data_received = 0;
  std::uint64_t acks_received = 0;
  std::uint64_t acks_sent = 0;
  std::uint64_t sacks_reported = 0;       // SACK bits ever reported
  std::uint64_t nacks_received = 0;
  std::uint64_t nacks_sent = 0;
  std::uint64_t abandons_sent = 0;
  std::uint64_t abandon_retransmissions = 0;
  std::uint64_t abandons_received = 0;
  std::uint64_t pings_sent = 0;
  std::uint64_t pings_received = 0;
  std::uint64_t pongs_sent = 0;
  std::uint64_t pongs_received = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t too_old = 0;
  std::uint64_t out_of_window = 0;
  std::uint64_t storage_full_drops = 0;
  std::uint64_t malformed = 0;
  std::uint64_t protocol_violations = 0;
  std::uint64_t stale_control = 0;
  std::uint64_t control_rate_dropped = 0;
  std::uint64_t control_queue_dropped = 0;
  std::uint64_t command_dropped = 0;
  std::uint64_t send_would_block = 0;
  std::uint64_t send_errors = 0;
  std::uint64_t unknown_session_datagrams = 0;
  std::uint64_t rtt_samples = 0;
  std::uint64_t stream_mismatch = 0;
  std::uint64_t dead_frame_drops = 0;
  std::uint64_t pre_sync_dropped = 0;
  std::uint64_t cascade_abandons = 0;

  PacketCounters& operator+=(const PacketCounters& o) {
    data_sent += o.data_sent; data_retransmissions += o.data_retransmissions;
    data_received += o.data_received; acks_received += o.acks_received;
    acks_sent += o.acks_sent; sacks_reported += o.sacks_reported;
    nacks_received += o.nacks_received; nacks_sent += o.nacks_sent;
    abandons_sent += o.abandons_sent; abandon_retransmissions += o.abandon_retransmissions;
    abandons_received += o.abandons_received; pings_sent += o.pings_sent;
    pings_received += o.pings_received; pongs_sent += o.pongs_sent;
    pongs_received += o.pongs_received; duplicates += o.duplicates;
    too_old += o.too_old; out_of_window += o.out_of_window;
    storage_full_drops += o.storage_full_drops; malformed += o.malformed;
    protocol_violations += o.protocol_violations; stale_control += o.stale_control;
    control_rate_dropped += o.control_rate_dropped; control_queue_dropped += o.control_queue_dropped;
    command_dropped += o.command_dropped; send_would_block += o.send_would_block;
    send_errors += o.send_errors; unknown_session_datagrams += o.unknown_session_datagrams;
    rtt_samples += o.rtt_samples;
    stream_mismatch += o.stream_mismatch;
    dead_frame_drops += o.dead_frame_drops;
    pre_sync_dropped += o.pre_sync_dropped;
    cascade_abandons += o.cascade_abandons;
    return *this;
  }
};

// ---------------------------------------------------------------------------
// Session-frame ledger for one session (spec §6 equation 2).
// ---------------------------------------------------------------------------
struct SessionFrameLedger {
  std::uint64_t ontime = 0;
  std::uint64_t late = 0;
  std::uint64_t abandoned = 0;
  std::uint64_t expired = 0;
  std::uint64_t pre_admission_dropped = 0;
  std::uint64_t in_flight = 0;

  std::uint64_t offered() const {
    return ontime + late + abandoned + expired + pre_admission_dropped + in_flight;
  }

  void admit() { ++in_flight; }

  void close(SessionFrameOutcome outcome) {
    if (outcome == SessionFrameOutcome::IN_FLIGHT) return;
    if (outcome == SessionFrameOutcome::PRE_ADMISSION_DROPPED) {
      // Offered but never admitted: not an in-flight transition (ADR-0005).
      ++pre_admission_dropped;
      return;
    }
    if (in_flight == 0) {
      // INV-20 breach: a frame closed twice or without admission.
      std::abort();
    }
    --in_flight;
    switch (outcome) {
      case SessionFrameOutcome::ONTIME: ++ontime; break;
      case SessionFrameOutcome::LATE: ++late; break;
      case SessionFrameOutcome::ABANDONED: ++abandoned; break;
      case SessionFrameOutcome::EXPIRED: ++expired; break;
      case SessionFrameOutcome::PRE_ADMISSION_DROPPED: ++pre_admission_dropped; break;
      case SessionFrameOutcome::IN_FLIGHT: break;
    }
  }
};

// ---------------------------------------------------------------------------
// Source ledger (spec §6 equation 1).
// ---------------------------------------------------------------------------
struct SourceLedger {
  std::uint64_t source_frames = 0;
  std::uint64_t capture_dropped = 0;
  std::uint64_t admitted_to_fanout = 0;

  void admitted() { ++admitted_to_fanout; }
  // Producer side knows the produced count; drops are the residual.
  void set_produced(std::uint64_t produced) {
    source_frames = produced;
    capture_dropped = produced - admitted_to_fanout;
  }
  // Consumer side knows the cross-thread drop counter; produced is derived.
  // Both directions maintain the conservation equation in one place.
  void fold_capture_drops(std::uint64_t dropped) {
    capture_dropped = dropped;
    source_frames = dropped + admitted_to_fanout;
  }
  bool conserved() const { return source_frames == capture_dropped + admitted_to_fanout; }
};

// ---------------------------------------------------------------------------
// Per-session summary included in snapshots.
// ---------------------------------------------------------------------------
struct SessionMetrics {
  std::uint32_t session_id = 0;
  std::uint32_t generation = 0;
  std::uint16_t stream_id = 0;
  Endpoint endpoint;
  std::string state;                     // "established", "dead", ...
  TeardownCause teardown_cause = TeardownCause::NONE;
  std::uint64_t srtt_ns = 0;
  std::uint64_t rto_ns = 0;
  std::uint32_t window_packets_in_flight = 0;
  SessionFrameLedger frames;
  PacketCounters packets;
  std::uint32_t keepalive_probes_unanswered = 0;
};

// ---------------------------------------------------------------------------
// Immutable snapshot (spec §3.4: value object, no pointers into live state).
// ---------------------------------------------------------------------------
struct MetricsSnapshot {
  MonoNs taken_at = 0;
  bool is_final = false;
  std::string clock_mode = "virtual";  // "virtual" | "real"
  std::uint64_t seed = 0;

  SourceLedger source;
  PacketCounters packets;

  std::uint64_t watchdog_violations = 0;
  std::uint64_t resource_exhausted = 0;
  std::uint64_t memory_high_water_bytes = 0;
  std::uint32_t queue_depth_hwm_capture = 0;
  std::uint32_t queue_depth_hwm_control = 0;
  std::uint32_t queue_depth_hwm_reorder = 0;
  std::uint32_t queue_depth_hwm_jitter = 0;
  std::uint32_t queue_depth_hwm_metrics = 0;
  std::uint32_t queue_depth_hwm_timer = 0;

  std::uint64_t playout_dropped = 0;    // jitter buffer overflow (client)
  std::uint64_t consumer_dropped = 0;   // consumer queue overflow (client)
  std::uint64_t delivered_ontime = 0;   // client playout classification
  std::uint64_t delivered_late = 0;
  std::uint64_t delivery_expired = 0;   // client-side expired frames
  std::uint64_t delivery_abandoned = 0; // client-side frames killed by ABANDON

  std::vector<SessionMetrics> sessions;
};

// Aggregate FDR over all sessions in a snapshot.
double session_fdr(const MetricsSnapshot& s);
std::uint64_t session_offered_total(const MetricsSnapshot& s);

// report.cpp — JSON serialization (metrics worker output) and the text
// summary printed by the apps.
std::string snapshot_to_json(const MetricsSnapshot& s);
std::string format_summary(const MetricsSnapshot& s);

}  // namespace rudp_rt
