// report.cpp — snapshot JSON serialization and text summaries (report gate).

#include "rudp_rt/metrics.hpp"

#include <cstdio>
#include <sstream>
#include <string>

namespace rudp_rt {

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      default: out += c;
    }
  }
  return out;
}

}  // namespace

std::string snapshot_to_json(const MetricsSnapshot& s) {
  std::ostringstream os;
  os << "{\"taken_at_ns\":" << s.taken_at << ",\"final\":"
     << (s.is_final ? "true" : "false") << ",\"clock_mode\":\""
     << json_escape(s.clock_mode) << "\",\"seed\":" << s.seed;
  os << ",\"source\":{\"frames\":" << s.source.source_frames
     << ",\"capture_dropped\":" << s.source.capture_dropped
     << ",\"admitted_to_fanout\":" << s.source.admitted_to_fanout
     << ",\"conserved\":"
     << (s.source.conserved() ? "true" : "false") << "}";
  const PacketCounters& p = s.packets;
  os << ",\"packets\":{\"data_sent\":" << p.data_sent
     << ",\"data_retransmissions\":" << p.data_retransmissions
     << ",\"data_received\":" << p.data_received
     << ",\"acks_received\":" << p.acks_received
     << ",\"acks_sent\":" << p.acks_sent
     << ",\"nacks_received\":" << p.nacks_received
     << ",\"nacks_sent\":" << p.nacks_sent
     << ",\"abandons_sent\":" << p.abandons_sent
     << ",\"abandons_received\":" << p.abandons_received
     << ",\"duplicates\":" << p.duplicates
     << ",\"too_old\":" << p.too_old
     << ",\"out_of_window\":" << p.out_of_window
     << ",\"storage_full_drops\":" << p.storage_full_drops
     << ",\"malformed\":" << p.malformed
     << ",\"protocol_violations\":" << p.protocol_violations
     << ",\"stale_control\":" << p.stale_control
     << ",\"control_queue_dropped\":" << p.control_queue_dropped
     << ",\"send_would_block\":" << p.send_would_block
     << ",\"send_errors\":" << p.send_errors
     << ",\"pre_sync_dropped\":" << p.pre_sync_dropped
     << ",\"cascade_abandons\":" << p.cascade_abandons << "}";
  os << ",\"watchdog_violations\":" << s.watchdog_violations
     << ",\"resource_exhausted\":" << s.resource_exhausted
     << ",\"memory_high_water_bytes\":" << s.memory_high_water_bytes
     << ",\"queue_depth_hwm\":{\"capture\":" << s.queue_depth_hwm_capture
     << ",\"control\":" << s.queue_depth_hwm_control
     << ",\"reorder\":" << s.queue_depth_hwm_reorder
     << ",\"jitter\":" << s.queue_depth_hwm_jitter
     << ",\"metrics\":" << s.queue_depth_hwm_metrics
     << ",\"timer\":" << s.queue_depth_hwm_timer << "}"
     << ",\"delivered_ontime\":" << s.delivered_ontime
     << ",\"delivered_late\":" << s.delivered_late
     << ",\"delivery_expired\":" << s.delivery_expired
     << ",\"delivery_abandoned\":" << s.delivery_abandoned
     << ",\"playout_dropped\":" << s.playout_dropped
     << ",\"consumer_dropped\":" << s.consumer_dropped;
  os << ",\"sessions\":[";
  bool first = true;
  for (const auto& sm : s.sessions) {
    if (!first) os << ",";
    first = false;
    os << "{\"id\":" << sm.session_id << ",\"generation\":" << sm.generation
       << ",\"stream\":" << sm.stream_id << ",\"state\":\""
       << json_escape(sm.state) << "\",\"teardown\":\""
       << to_string(sm.teardown_cause) << "\",\"srtt_ns\":" << sm.srtt_ns
       << ",\"rto_ns\":" << sm.rto_ns
       << ",\"window_in_flight\":" << sm.window_packets_in_flight
       << ",\"frames\":{\"ontime\":" << sm.frames.ontime
       << ",\"late\":" << sm.frames.late
       << ",\"abandoned\":" << sm.frames.abandoned
       << ",\"expired\":" << sm.frames.expired
       << ",\"pre_admission_dropped\":" << sm.frames.pre_admission_dropped
       << ",\"in_flight\":" << sm.frames.in_flight << "}}";
  }
  os << "]";
  os << ",\"session_offered\":" << session_offered_total(s)
     << ",\"session_fdr\":" << session_fdr(s);
  os << "}\n";
  return os.str();
}

std::string format_summary(const MetricsSnapshot& s) {
  std::ostringstream os;
  const char* tag = s.is_final ? "FINAL" : "snapshot";
  os << "== " << tag << " (clock=" << s.clock_mode << ") ==\n";
  os << "source ledger: frames=" << s.source.source_frames
     << " capture_dropped=" << s.source.capture_dropped
     << " admitted=" << s.source.admitted_to_fanout
     << " conserved=" << (s.source.conserved() ? "yes" : "NO") << "\n";
  for (const auto& sm : s.sessions) {
    os << "session " << sm.session_id << " (gen " << sm.generation
       << ", stream " << sm.stream_id << ", " << sm.state
       << ", teardown=" << to_string(sm.teardown_cause) << "): offered="
       << sm.frames.offered() << " ontime=" << sm.frames.ontime
       << " late=" << sm.frames.late << " abandoned=" << sm.frames.abandoned
       << " expired=" << sm.frames.expired
       << " pre_admission=" << sm.frames.pre_admission_dropped
       << " in_flight=" << sm.frames.in_flight
       << " srtt_ms=" << static_cast<double>(sm.srtt_ns) / 1e6 << "\n";
  }
  os << "aggregate FDR=" << session_fdr(s)
     << " offered=" << session_offered_total(s) << "\n";
  os << "client playout: ontime=" << s.delivered_ontime
     << " late=" << s.delivered_late << " expired=" << s.delivery_expired
     << " abandoned=" << s.delivery_abandoned
     << " playout_dropped=" << s.playout_dropped << "\n";
  os << "packets: data_sent=" << s.packets.data_sent
     << " retrans=" << s.packets.data_retransmissions
     << " acks=" << s.packets.acks_received
     << " nacks=" << s.packets.nacks_received
     << " abandons_sent=" << s.packets.abandons_sent
     << " dup=" << s.packets.duplicates
     << " malformed=" << s.packets.malformed
     << " violations=" << s.packets.protocol_violations << "\n";
  os << "watchdog_violations=" << s.watchdog_violations
     << " resource_exhausted=" << s.resource_exhausted
     << " mem_hwm_bytes=" << s.memory_high_water_bytes << "\n";
  return os.str();
}

}  // namespace rudp_rt
