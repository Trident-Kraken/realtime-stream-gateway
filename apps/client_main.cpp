// client_main.cpp — rudp_client entry point (reference receiver, ADR-0015).

#include "rudp_rt/metrics.hpp"
#include "rudp_rt/runtime.hpp"

#include <cstring>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <arpa/inet.h>

namespace {

void usage() {
  std::cerr
      << "usage: rudp_client --host ADDR --port N [--config FILE]\n"
      << "                  [--stream N] [--duration MS] [--metrics-out FILE]\n";
}

std::uint16_t parse_u16_arg(const std::string& flag, const std::string& val,
                            std::uint16_t min_val) {
  std::size_t used = 0;
  const long long v = std::stoll(val, &used, 10);
  if (used != val.size() || v < static_cast<long long>(min_val) || v > 65535) {
    throw std::invalid_argument("invalid value for " + flag + ": '" + val + "'");
  }
  return static_cast<std::uint16_t>(v);
}

std::uint64_t parse_u64_arg(const std::string& flag, const std::string& val) {
  if (val.empty() || val.front() == '-') {
    throw std::invalid_argument("invalid value for " + flag + ": '" + val + "'");
  }
  std::size_t used = 0;
  const std::uint64_t v = std::stoull(val, &used, 10);
  if (used != val.size()) {
    throw std::invalid_argument("invalid value for " + flag + ": '" + val + "'");
  }
  return v;
}

}  // namespace

int main(int argc, char** argv) {
  rudp_rt::Config cfg;
  std::string host = "127.0.0.1";
  std::optional<std::uint16_t> opt_port;
  std::optional<std::uint16_t> opt_stream;
  std::string config_path;
  std::string metrics_path;
  std::uint64_t duration_ms = 0;
  try {
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      auto next = [&]() -> std::string {
        if (i + 1 >= argc) {
          throw std::invalid_argument("missing value for " + arg);
        }
        return argv[++i];
      };
      if (arg == "--config") config_path = next();
      else if (arg == "--host") host = next();
      else if (arg == "--port") opt_port = parse_u16_arg(arg, next(), 1);
      else if (arg == "--stream") opt_stream = parse_u16_arg(arg, next(), 1);
      else if (arg == "--duration") duration_ms = parse_u64_arg(arg, next());
      else if (arg == "--metrics-out") metrics_path = next();
      else if (arg == "--help") { usage(); return 0; }
      else {
        std::cerr << "unknown argument: " << arg << "\n";
        usage();
        return 2;
      }
    }
  } catch (const std::exception& e) {
    std::cerr << "rudp_client: " << e.what() << "\n";
    return 2;
  }
  if (duration_ms == 0) {
    std::cerr << "rudp_client: --duration is required (ms)\n";
    usage();
    return 2;
  }

  try {
    if (!config_path.empty()) {
      cfg = rudp_rt::load_config_file(config_path, cfg);
    }
    if (opt_stream) cfg.stream_id = *opt_stream;
    const std::uint16_t port = opt_port.value_or(cfg.port);

    rudp_rt::Endpoint gateway{};
    in_addr addr{};
    if (::inet_pton(AF_INET, host.c_str(), &addr) != 1) {
      throw std::runtime_error("invalid --host: " + host);
    }
    gateway.ipv4_be = addr.s_addr;
    gateway.port_be = htons(port);

    rudp_rt::Client client(cfg, gateway, cfg.stream_id);
    if (!metrics_path.empty()) client.set_metrics_path(metrics_path);
    client.start();
    const rudp_rt::MetricsSnapshot final_snapshot =
        client.run(static_cast<rudp_rt::MonoNs>(duration_ms) *
                   rudp_rt::kNsPerMs);
    for (int i = 0; i < 200 && !client.metrics_final_acked(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout << rudp_rt::format_summary(final_snapshot);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "rudp_client: " << e.what() << "\n";
    return 1;
  }
}
