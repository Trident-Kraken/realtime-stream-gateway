// config.cpp — flat TOML subset loader and startup validation (ADR-0014).

#include "rudp_rt/config.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace rudp_rt {

MonoNs parse_duration(const std::string& text) {
  // Accepts "Ns", "Nms", "Nus", "Nm", "Nh", or a bare integer (nanoseconds).
  std::size_t i = 0;
  while (i < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.' ||
          text[i] == '-' || text[i] == '+')) {
    ++i;
  }
  if (i == 0) throw std::runtime_error("invalid duration: '" + text + "'");
  const double value = std::stod(text.substr(0, i));
  const std::string unit = text.substr(i);
  double ns = 0;
  if (unit == "ns") ns = value;
  else if (unit == "us") ns = value * 1e3;
  else if (unit == "ms") ns = value * 1e6;
  else if (unit == "s") ns = value * 1e9;
  else if (unit == "m") ns = value * 60e9;
  else if (unit == "h") ns = value * 3600e9;
  else if (unit.empty()) ns = value;
  else throw std::runtime_error("invalid duration unit: '" + text + "'");
  return static_cast<MonoNs>(ns);
}

namespace {

std::string trim(const std::string& s) {
  std::size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

struct KeyValue {
  std::string section;
  std::string key;
  std::string value;
};

std::vector<KeyValue> parse_toml_subset(const std::string& text) {
  std::vector<KeyValue> out;
  std::string section;
  std::istringstream is(text);
  std::string line;
  int line_no = 0;
  while (std::getline(is, line)) {
    ++line_no;
    // strip comments outside quotes
    bool in_quote = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
      if (line[i] == '"') in_quote = !in_quote;
      if (line[i] == '#' && !in_quote) {
        line = line.substr(0, i);
        break;
      }
    }
    line = trim(line);
    if (line.empty()) continue;
    if (line.front() == '[') {
      if (line.back() != ']')
        throw std::runtime_error("config line " + std::to_string(line_no) +
                                 ": malformed section header");
      section = trim(line.substr(1, line.size() - 2));
      continue;
    }
    const auto eq = line.find('=');
    if (eq == std::string::npos)
      throw std::runtime_error("config line " + std::to_string(line_no) +
                               ": expected key = value");
    KeyValue kv;
    kv.section = section;
    kv.key = trim(line.substr(0, eq));
    kv.value = trim(line.substr(eq + 1));
    if (kv.value.size() >= 2 && kv.value.front() == '"' &&
        kv.value.back() == '"') {
      kv.value = kv.value.substr(1, kv.value.size() - 2);
    }
    if (kv.key.empty())
      throw std::runtime_error("config line " + std::to_string(line_no) +
                               ": empty key");
    out.push_back(std::move(kv));
  }
  return out;
}

int parse_int(const KeyValue& kv) {
  try {
    return std::stoi(kv.value);
  } catch (const std::exception&) {
    throw std::runtime_error("config: " + kv.section + "." + kv.key +
                             ": expected integer, got '" + kv.value + "'");
  }
}
std::int64_t parse_int64(const KeyValue& kv) {
  try {
    return std::stoll(kv.value);
  } catch (const std::exception&) {
    throw std::runtime_error("config: " + kv.section + "." + kv.key +
                             ": expected integer, got '" + kv.value + "'");
  }
}
bool parse_bool(const KeyValue& kv) {
  if (kv.value == "true") return true;
  if (kv.value == "false") return false;
  throw std::runtime_error("config: " + kv.section + "." + kv.key +
                           ": expected bool, got '" + kv.value + "'");
}
std::uint32_t parse_u32(const KeyValue& kv) {
  const std::int64_t v = parse_int64(kv);
  if (v < 0 || v > 0xFFFFFFFFll)
    throw std::runtime_error("config: " + kv.section + "." + kv.key +
                             ": out of uint32 range");
  return static_cast<std::uint32_t>(v);
}

}  // namespace

Config load_config_from_string(const std::string& toml, Config base) {
  Config cfg = std::move(base);
  for (const auto& kv : parse_toml_subset(toml)) {
    const std::string full = kv.section + "." + kv.key;
    if (kv.section == "gateway") {
      if (kv.key == "bind_address") cfg.bind_address = kv.value;
      else if (kv.key == "port") cfg.port = static_cast<std::uint16_t>(parse_int(kv));
      else if (kv.key == "stream_id") cfg.stream_id = static_cast<std::uint16_t>(parse_int(kv));
      else if (kv.key == "max_sessions") cfg.max_sessions = parse_int(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "media") {
      if (kv.key == "fps") cfg.fps = parse_u32(kv);
      else if (kv.key == "frame_bytes") cfg.frame_bytes = parse_int(kv);
      else if (kv.key == "media_payload_bytes") cfg.media_payload_bytes = parse_int(kv);
      else if (kv.key == "deadline_budget") cfg.deadline_budget = parse_duration(kv.value);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "transport") {
      if (kv.key == "window_w") cfg.window_w = parse_int(kv);
      else if (kv.key == "max_frame_packets") cfg.max_frame_packets = parse_int(kv);
      else if (kv.key == "rto_min") cfg.rto_min = parse_duration(kv.value);
      else if (kv.key == "rto_max") cfg.rto_max = parse_duration(kv.value);
      else if (kv.key == "srtt_initial") cfg.srtt_initial = parse_duration(kv.value);
      else if (kv.key == "rttvar_initial") cfg.rttvar_initial = parse_duration(kv.value);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "client") {
      if (kv.key == "playout_delay") cfg.playout_delay = parse_duration(kv.value);
      else if (kv.key == "jitter_buffer_frames") cfg.jitter_buffer_frames = parse_int(kv);
      else if (kv.key == "reorder_buffer_packets") cfg.reorder_buffer_packets = parse_int(kv);
      else if (kv.key == "auto_rejoin") cfg.auto_rejoin = parse_bool(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "queues") {
      if (kv.key == "capture_queue_frames") cfg.capture_queue_frames = parse_int(kv);
      else if (kv.key == "control_queue_datagrams") cfg.control_queue_datagrams = parse_int(kv);
      else if (kv.key == "metrics_queue_snapshots") cfg.metrics_queue_snapshots = parse_int(kv);
      else if (kv.key == "timer_capacity") cfg.timer_capacity = parse_int(kv);
      else if (kv.key == "consumer_queue_frames") cfg.consumer_queue_frames = parse_int(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "scheduling") {
      if (kv.key == "turn_datagram_limit") cfg.turn_datagram_limit = parse_int64(kv);
      else if (kv.key == "turn_byte_limit") cfg.turn_byte_limit = parse_int64(kv);
      else if (kv.key == "token_rate_bytes_per_sec") cfg.token_rate_bytes_per_sec = parse_int64(kv);
      else if (kv.key == "token_burst_bytes") cfg.token_burst_bytes = parse_int64(kv);
      else if (kv.key == "control_reserve_bytes") cfg.control_reserve_bytes = parse_int64(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "keepalive") {
      if (kv.key == "silence_after") cfg.keepalive_silence_after = parse_duration(kv.value);
      else if (kv.key == "min_interval") cfg.keepalive_min_interval = parse_duration(kv.value);
      else if (kv.key == "max_probes") cfg.keepalive_max_probes = parse_int(kv);
      else if (kv.key == "ack_starvation_rto_intervals")
        cfg.ack_starvation_rto_intervals = parse_int(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "sockets") {
      if (kv.key == "so_rcvbuf_request") cfg.so_rcvbuf_request = parse_int(kv);
      else if (kv.key == "so_sndbuf_request") cfg.so_sndbuf_request = parse_int(kv);
      else if (kv.key == "so_rcvbuf_min") cfg.so_rcvbuf_min = parse_int(kv);
      else if (kv.key == "so_sndbuf_min") cfg.so_sndbuf_min = parse_int(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "runtime") {
      if (kv.key == "drain_deadline") cfg.drain_deadline = parse_duration(kv.value);
      else if (kv.key == "metrics_interval") cfg.metrics_interval = parse_duration(kv.value);
      else throw std::runtime_error("unknown config key: " + full);
    } else if (kv.section == "experiment") {
      if (kv.key == "seed") cfg.seed = static_cast<std::uint64_t>(parse_int64(kv));
      else if (kv.key == "duration") cfg.sim_duration = parse_duration(kv.value);
      else if (kv.key == "warmup_frames") cfg.warmup_frames = parse_u32(kv);
      else if (kv.key == "measured_frames") cfg.measured_frames = parse_u32(kv);
      else if (kv.key == "bench_clients") cfg.bench_clients = parse_int(kv);
      else if (kv.key == "deadline_mode") cfg.deadline_mode = parse_bool(kv);
      else throw std::runtime_error("unknown config key: " + full);
    } else {
      throw std::runtime_error("unknown config section: " + kv.section);
    }
  }
  return cfg;
}

Config load_config_file(const std::string& path, Config base) {
  std::ifstream file(path);
  if (!file) throw std::runtime_error("cannot open config file: " + path);
  std::ostringstream os;
  os << file.rdbuf();
  return load_config_from_string(os.str(), std::move(base));
}

std::vector<std::string> validate_config(const Config& cfg) {
  std::vector<std::string> errors;
  const auto add = [&errors](bool ok, const std::string& msg) {
    if (!ok) errors.push_back(msg);
  };

  add(cfg.stream_id != 0, "stream_id must be non-zero");
  add(cfg.max_sessions >= 1, "max_sessions must be >= 1");
  add(cfg.window_w >= 1 && cfg.window_w <= 64, "window_w must be in [1, 64]");
  add(cfg.max_frame_packets >= 1 && cfg.max_frame_packets <= cfg.window_w,
      "max_frame_packets must be in [1, window_w]");
  add(cfg.media_payload_bytes >= 1 && cfg.media_payload_bytes <= 1200,
      "media_payload_bytes must be in [1, 1200]");
  add(cfg.frame_bytes >= 1 && cfg.frame_bytes <= cfg.media_payload_bytes * cfg.max_frame_packets,
      "frame_bytes must be <= media_payload_bytes * max_frame_packets");
  add(cfg.deadline_budget > 0, "deadline_budget must be > 0");
  add(cfg.rto_min > 0, "rto_min must be > 0");
  add(cfg.rto_max >= cfg.rto_min, "rto_max must be >= rto_min");
  add(cfg.playout_delay >= 0, "playout_delay must be >= 0");
  add(cfg.capture_queue_frames > 0, "capture_queue_frames must be > 0");
  add(cfg.control_queue_datagrams > 0, "control_queue_datagrams must be > 0");
  add(cfg.metrics_queue_snapshots > 0, "metrics_queue_snapshots must be > 0");
  add(cfg.timer_capacity > 0, "timer_capacity must be > 0");
  add(cfg.jitter_buffer_frames > 0, "jitter_buffer_frames must be > 0");
  add(cfg.reorder_buffer_packets > 0, "reorder_buffer_packets must be > 0");
  add(cfg.consumer_queue_frames > 0, "consumer_queue_frames must be > 0");
  add(cfg.control_reserve_bytes <
          static_cast<std::int64_t>(cfg.control_queue_datagrams) * 1240,
      "control_reserve_bytes must be < control_queue_datagrams * 1240");
  add(cfg.token_burst_bytes >= 1240,
      "token_burst_bytes must be >= one maximum datagram (1240)");
  add(cfg.token_rate_bytes_per_sec > 0, "token_rate_bytes_per_sec must be > 0");
  add(cfg.turn_datagram_limit > 0, "turn_datagram_limit must be > 0");
  add(cfg.turn_byte_limit > 0, "turn_byte_limit must be > 0");
  add(cfg.keepalive_silence_after > 0, "keepalive silence_after must be > 0");
  add(cfg.keepalive_min_interval >= cfg.keepalive_silence_after,
      "keepalive min_interval must be >= silence_after");
  add(cfg.keepalive_max_probes >= 1, "keepalive max_probes must be >= 1");
  add(cfg.ack_starvation_rto_intervals >= 1,
      "ack_starvation_rto_intervals must be >= 1");
  add(cfg.so_rcvbuf_request > 0 && cfg.so_sndbuf_request > 0,
      "socket buffer requests must be > 0");
  add(cfg.so_rcvbuf_min > 0 && cfg.so_sndbuf_min > 0,
      "socket buffer minimums must be > 0");
  add(cfg.so_rcvbuf_min <= cfg.so_rcvbuf_request &&
          cfg.so_sndbuf_min <= cfg.so_sndbuf_request,
      "socket buffer minimums must not exceed requests");
  add(cfg.drain_deadline > 0, "drain_deadline must be > 0");
  add(cfg.metrics_interval > 0, "metrics_interval must be > 0");
  add(cfg.timer_capacity >= cfg.max_sessions * 8,
      "timer_capacity must be >= max_sessions * 8");
  add(cfg.fps > 0 && cfg.fps <= 1000, "fps must be in (0, 1000]");
  return errors;
}

}  // namespace rudp_rt
