// sim_main.cpp — rudp_sim: deterministic in-process virtual-clock/virtual-
// network runner used by integration and fault tests (spec §2.1, Phase 2+).

#include "rudp_rt/metrics.hpp"
#include "rudp_rt/sim.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <arpa/inet.h>
#include <iostream>
#include <string>
#include <vector>

using namespace rudp_rt;

namespace {

std::string json_escape(const std::string& s) {
  std::string out;
  for (const char c : s) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string scenario = "basic";
  std::uint64_t seed = 0x5EED;
  double loss = 0.0;
  int burst = 0;
  double down_loss = -1.0;
  int down_burst = -1;
  double control_loss = 0.0;
  double delay_ms = 5.0;   // one-way; 10 ms RTT with symmetric paths
  double down_delay_ms = -1.0;
  double jitter_ms = 0.0;
  double reorder = 0.0;
  double duplicate = 0.0;
  int clients = 1;
  std::uint32_t frames = 300;
  std::uint32_t warmup = 0;
  std::uint64_t duration_ms = 0;  // 0 = derived from frame count
  bool deadline_mode = true;
  std::string json_path;
  std::string trace_path;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--scenario") scenario = next();
    else if (arg == "--seed") seed = std::stoull(next());
    else if (arg == "--loss") loss = std::stod(next());
    else if (arg == "--burst") burst = std::stoi(next());
    else if (arg == "--down-loss") down_loss = std::stod(next());
    else if (arg == "--down-burst") down_burst = std::stoi(next());
    else if (arg == "--control-loss") control_loss = std::stod(next());
    else if (arg == "--delay") delay_ms = std::stod(next());
    else if (arg == "--down-delay") down_delay_ms = std::stod(next());
    else if (arg == "--jitter") jitter_ms = std::stod(next());
    else if (arg == "--reorder") reorder = std::stod(next());
    else if (arg == "--duplicate") duplicate = std::stod(next());
    else if (arg == "--clients") clients = std::stoi(next());
    else if (arg == "--frames") frames = static_cast<std::uint32_t>(std::stoul(next()));
    else if (arg == "--warmup") warmup = static_cast<std::uint32_t>(std::stoul(next()));
    else if (arg == "--duration") duration_ms = std::stoull(next());
    else if (arg == "--no-deadline") deadline_mode = false;
    else if (arg == "--json") json_path = next();
    else if (arg == "--trace") trace_path = next();
    else if (arg == "--help") {
      std::cout << "usage: rudp_sim [options]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }
  if (down_loss < 0) down_loss = loss;
  if (down_burst < 0) down_burst = burst;
  if (down_delay_ms < 0) down_delay_ms = delay_ms;

  rudp_rt::Config cfg;
  cfg.deadline_mode = deadline_mode;
  cfg.max_frame_packets = 10;
  cfg.frame_bytes = 12000;
  cfg.media_payload_bytes = 1200;

  rudp_rt::SyntheticSource::Params sp;
  sp.stream_id = cfg.stream_id;
  sp.ssrc = 0x5EED0001;
  sp.media_bytes = static_cast<std::uint32_t>(cfg.frame_bytes);
  sp.fps = 30;
  sp.deadline_budget = cfg.deadline_budget;
  sp.start_time = 0;
  rudp_rt::SimCluster cluster(cfg, seed, seed ^ 0xA5A5A5A5u, sp);

  const MonoNs up_delay =
      static_cast<MonoNs>(delay_ms * 1e6);
  const MonoNs down_delay =
      static_cast<MonoNs>(down_delay_ms * 1e6);
  const MonoNs jitter =
      static_cast<MonoNs>(jitter_ms * 1e6);

  rudp_rt::DirectionFaults up;  // client -> gateway (ACK/NACK path)
  up.data_loss = loss;          // ACKs ride the "data" classification for the
  up.control_loss = control_loss;  // uplink only when configured explicitly
  up.burst_length = burst;
  up.base_delay = up_delay;
  up.jitter = jitter;
  up.reorder = reorder;
  up.duplicate = duplicate;

  rudp_rt::DirectionFaults down;  // gateway -> clients (DATA path)
  down.data_loss = down_loss;
  down.control_loss = control_loss;
  down.burst_length = down_burst;
  down.base_delay = down_delay;
  down.jitter = jitter;
  down.reorder = reorder;
  down.duplicate = duplicate;

  for (int i = 0; i < clients; ++i) {
    cluster.add_client(seed + 1000u + static_cast<std::uint64_t>(i));
    cluster.set_client_route(static_cast<std::size_t>(i), up);
  }
  cluster.set_gateway_route(down);
  cluster.enable_latencies();

  const MonoNs run_duration =
      duration_ms > 0
          ? static_cast<MonoNs>(duration_ms * rudp_rt::kNsPerMs)
          : static_cast<MonoNs>(
                (static_cast<std::uint64_t>(frames) + 400) * 1000 *
                rudp_rt::kNsPerMs / 30);  // 30 fps window + tail

  cluster.run(run_duration);
  const std::uint64_t offered_frames = cluster.fed;

  // ------------------------------------------------------------------
  // Results + invariant checks (conservation, watchdog, sequence terminality)
  // ------------------------------------------------------------------
  bool invariants_ok = true;
  auto check = [&](bool ok, const char* what) {
    if (!ok) {
      invariants_ok = false;
      std::cerr << "INVARIANT VIOLATION: " << what << "\n";
    }
  };

  // Capture-feed conservation: every frame the source produced was offered
  // to the gateway (the sim has no capture worker, so nothing drops).
  check(cluster.fed == cluster.source.frames_produced(),
        "source feed conservation");
  check(cluster.gateway.watchdog_violations() == 0, "watchdog violations");

  const auto sessions = cluster.gateway.collect_sessions();
  std::uint64_t offered = 0, ontime = 0, late = 0, abandoned = 0,
                 expired = 0, pre_admission = 0, in_flight = 0;
  for (const auto& sm : sessions) {
    offered += sm.frames.offered();
    ontime += sm.frames.ontime;
    late += sm.frames.late;
    abandoned += sm.frames.abandoned;
    expired += sm.frames.expired;
    pre_admission += sm.frames.pre_admission_dropped;
    in_flight += sm.frames.in_flight;
    check(sm.frames.offered() ==
              sm.frames.ontime + sm.frames.late + sm.frames.abandoned +
                  sm.frames.expired + sm.frames.pre_admission_dropped +
                  sm.frames.in_flight,
          "session ledger conservation");
  }

  // Client-side playout aggregation.
  std::uint64_t c_ontime = 0, c_late = 0, c_expired = 0, c_abandoned = 0;
  std::vector<MonoNs> latencies;
  std::vector<std::pair<int, std::uint32_t>> delivered_ids;
  for (int i = 0; i < clients; ++i) {
    const auto& node = *cluster.clients[static_cast<std::size_t>(i)].node;
    c_ontime += node.delivered_ontime();
    c_late += node.delivered_late();
    c_expired += node.delivery_expired();
    c_abandoned += node.delivery_abandoned();
    for (const MonoNs l : node.latencies()) latencies.push_back(l);
    for (const std::uint32_t fid : node.delivered_frame_ids()) {
      delivered_ids.push_back({i, fid});
    }
  }

  // Warmup-filtered FDR (frames >= warmup): count client deliveries.
  std::uint64_t measured_delivered = 0;
  for (const auto& [ci, fid] : delivered_ids) {
    (void)ci;
    if (fid >= warmup) ++measured_delivered;
  }

  const double fdr_session =
      offered > 0 ? static_cast<double>(ontime) / static_cast<double>(offered)
                  : 0.0;
  const double fdr_playout =
      (c_ontime + c_late + c_expired + c_abandoned) > 0
          ? static_cast<double>(c_ontime) /
                static_cast<double>(c_ontime + c_late + c_expired +
                                    c_abandoned)
          : 0.0;

  auto percentile = [&latencies](double p) -> double {
    if (latencies.empty()) return 0.0;
    std::vector<MonoNs> sorted = latencies;
    std::sort(sorted.begin(), sorted.end());
    const std::size_t idx = static_cast<std::size_t>(
        p * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[idx]) / 1e6;
  };

  std::ostringstream json;
  json << "{\n";
  json << "  \"scenario\": \"" << json_escape(scenario) << "\",\n";
  json << "  \"seed\": " << seed << ",\n";
  json << "  \"params\": {\"loss\":" << loss << ",\"burst\":" << burst
       << ",\"down_loss\":" << down_loss << ",\"down_burst\":" << down_burst
       << ",\"control_loss\":" << control_loss << ",\"delay_ms\":" << delay_ms
       << ",\"down_delay_ms\":" << down_delay_ms << ",\"jitter_ms\":"
       << jitter_ms << ",\"reorder\":" << reorder << ",\"duplicate\":"
       << duplicate << ",\"clients\":" << clients << ",\"frames\":" << frames
       << ",\"deadline_mode\":" << (deadline_mode ? "true" : "false") << "},\n";
  json << "  \"simulated_ns\": " << cluster.clock.now() << ",\n";
  json << "  \"offered_frames\": " << offered_frames << ",\n";
  json << "  \"session_ledger\": {\"offered\":" << offered << ",\"ontime\":"
       << ontime << ",\"late\":" << late << ",\"abandoned\":" << abandoned
       << ",\"expired\":" << expired << ",\"pre_admission_dropped\":"
       << pre_admission << ",\"in_flight\":" << in_flight << "},\n";
  json << "  \"fdr_session\": " << fdr_session << ",\n";
  json << "  \"client_playout\": {\"ontime\":" << c_ontime << ",\"late\":"
       << c_late << ",\"expired\":" << c_expired << ",\"abandoned\":"
       << c_abandoned << ",\"playout_dropped\":"
       << [&, clients] {
            std::uint64_t d = 0;
            for (int i = 0; i < clients; ++i) {
              d += cluster.clients[static_cast<std::size_t>(i)]
                       .node->playout_dropped();
            }
            return d;
          }() << ",\"measured_delivered\":" << measured_delivered << "},\n";
  json << "  \"fdr_playout\": " << fdr_playout << ",\n";
  json << "  \"latency_ms\": {\"p50\":" << percentile(0.50)
       << ",\"p95\":" << percentile(0.95) << ",\"p99\":" << percentile(0.99)
       << ",\"max\":" << percentile(1.0) << "},\n";
  {
    std::uint64_t g_abandons = 0, g_data = 0, g_acks = 0;
    for (const auto& sm : sessions) {
      g_abandons += sm.packets.abandons_sent;
      g_data += sm.packets.data_sent;
      g_acks += sm.packets.acks_received;
    }
    std::uint64_t c_abandons = 0, c_acks = 0;
    for (int i = 0; i < clients; ++i) {
      const auto& node = *cluster.clients[static_cast<std::size_t>(i)].node;
      c_abandons += node.counters_.abandons_received;
      c_acks += node.counters_.acks_sent;
    }
    json << "  \"packets\": {\"gw_data_sent\":" << g_data
         << ",\"gw_acks_received\":" << g_acks
         << ",\"gw_abandons_sent\":" << g_abandons
         << ",\"c_acks_sent\":" << c_acks
         << ",\"c_abandons_received\":" << c_abandons << "},\n";
  }
  json << "  \"net\": {\"dropped\":" << cluster.net.stats().dropped
       << ",\"duplicated\":" << cluster.net.stats().duplicated
       << ",\"delivered\":" << cluster.driver.net_stats().delivered << "},\n";
  {
    std::size_t ctrl_hwm = 0;
    std::uint32_t jitter_hwm = 0, reorder_hwm = 0;
    for (int i = 0; i < clients; ++i) {
      const auto& node = *cluster.clients[static_cast<std::size_t>(i)].node;
      ctrl_hwm = std::max(ctrl_hwm, node.control_hwm());
      jitter_hwm = std::max(jitter_hwm,
                            static_cast<std::uint32_t>(node.jitter_depth()));
      reorder_hwm = std::max(reorder_hwm,
                             static_cast<std::uint32_t>(node.reorder_depth()));
    }
    json << "  \"queue_depth\": {\"client_control_hwm\":" << ctrl_hwm
         << ",\"reorder\":" << reorder_hwm << ",\"jitter\":" << jitter_hwm
         << ",\"gateway_control_hwm\":" << cluster.gateway.control_hwm()
         << "},\n";
  }
  json << "  \"invariants_ok\": " << (invariants_ok ? "true" : "false") << "\n";
  json << "}\n";

  if (!json_path.empty()) {
    std::ofstream out(json_path);
    out << json.str();
  }
  if (!trace_path.empty()) {
    // Replayable trace: scenario parameters + seed + outcomes (spec Phase 5).
    std::ofstream out(trace_path);
    out << "# trace: re-run `rudp_sim` with identical parameters and seed to\n"
        << "# reproduce this record bit-for-bit (deterministic virtual clock).\n";
    out << json.str();
  }
  std::cout << json.str();
  return invariants_ok ? 0 : 1;
}
