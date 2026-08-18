#pragma once
// rudp_rt/config.hpp — validated configuration (ADR-0014).
//
// Configuration is validated exactly once before any worker is created.
// Invalid values or cross-constraints fail startup with every violation
// listed; nothing is clamped silently. Only the socket-buffer grant is
// checked against the OS at bind time (see socket_network.cpp).

#include "rudp_rt/types.hpp"

#include <string>
#include <vector>

namespace rudp_rt {

struct Config {
  // --- gateway / network ---
  std::string bind_address = "127.0.0.1";
  std::uint16_t port = 50000;
  std::uint16_t stream_id = 1;
  std::uint32_t ssrc = 0;
  int max_sessions = 16;

  // --- media ---
  std::uint32_t fps = 30;
  int frame_bytes = 12000;          // media bytes per source frame (spec §2.3)
  int media_payload_bytes = 1200;   // fragment bound; wire max is 1200
  MonoNs deadline_budget = ns_from_ms(100);

  // --- transport ---
  int window_w = 64;
  int max_frame_packets = 10;
  MonoNs rto_min = ns_from_ms(50);
  MonoNs rto_max = ns_from_ms(1000);
  MonoNs srtt_initial = ns_from_ms(25);
  MonoNs rttvar_initial = ns_from_ms(25) / 2;

  // --- client ---
  MonoNs playout_delay = ns_from_ms(60);
  int jitter_buffer_frames = 64;
  int reorder_buffer_packets = 256;
  bool auto_rejoin = true;

  // --- queues (ADR-0007) ---
  int capture_queue_frames = 256;
  int control_queue_datagrams = 128;
  int metrics_queue_snapshots = 32;
  int timer_capacity = 4096;
  int consumer_queue_frames = 128;

  // --- scheduling (ADR-0008) ---
  std::int64_t turn_datagram_limit = 32;
  std::int64_t turn_byte_limit = 32768;
  std::int64_t token_rate_bytes_per_sec = 4 * 1024 * 1024;
  std::int64_t token_burst_bytes = 64 * 1024;
  std::int64_t control_reserve_bytes = 16 * 1024;

  // --- keepalive (spec §4.3) ---
  MonoNs keepalive_silence_after = ns_from_ms(200);
  MonoNs keepalive_min_interval = ns_from_ms(500);
  int keepalive_max_probes = 3;
  int ack_starvation_rto_intervals = 3;

  // --- sockets ---
  int so_rcvbuf_request = 4 * 1024 * 1024;
  int so_sndbuf_request = 4 * 1024 * 1024;
  int so_rcvbuf_min = 256 * 1024;
  int so_sndbuf_min = 256 * 1024;

  // --- runtime ---
  MonoNs drain_deadline = ns_from_ms(1000);
  MonoNs metrics_interval = ns_from_ms(1000);

  // --- simulator / benchmark extension (not part of the wire) ---
  std::uint64_t seed = 0x5EED;
  MonoNs sim_duration = 0;      // 0 = run to quiescence/frames
  std::uint32_t warmup_frames = 100;
  std::uint32_t measured_frames = 10000;
  int bench_clients = 3;
  bool deadline_mode = true;    // false = always-retransmit arm (ADR-0012)

  // Derived: DATA payload per packet (RTP header + fragment bound).
  int data_payload_bytes() const { return 12 + media_payload_bytes; }
  int datagram_bytes() const { return 28 + data_payload_bytes(); }
};

// Returns every violated constraint; an empty list means the config is valid.
// Cross-constraints follow spec §5 and ADR-0014 exactly.
std::vector<std::string> validate_config(const Config& cfg);

// Loads a flat TOML subset (ADR-0014) into a Config starting from `base`.
// Throws std::runtime_error on parse errors or unknown keys.
Config load_config_from_string(const std::string& toml, Config base = {});
Config load_config_file(const std::string& path, Config base = {});

// Duration parser: "100ms", "1s", "500us", "2m", or a bare nanosecond int.
MonoNs parse_duration(const std::string& text);

}  // namespace rudp_rt
