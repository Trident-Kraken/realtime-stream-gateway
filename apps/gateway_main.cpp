// gateway_main.cpp — rudp_gateway entry point.

#include "rudp_rt/metrics.hpp"
#include "rudp_rt/runtime.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#include <arpa/inet.h>

namespace {

void usage() {
  std::cerr
      << "usage: rudp_gateway [--config FILE] [--bind ADDR] [--port N]\n"
      << "                    [--stream N] [--duration MS] [--metrics-out FILE]\n"
      << "  --duration 0 runs until SIGINT/SIGTERM (default: 0)\n";
}

}  // namespace

int main(int argc, char** argv) {
  rudp_rt::Config cfg;
  std::string config_path;
  std::string metrics_path;
  std::uint64_t duration_ms = 0;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--config") config_path = next();
    else if (arg == "--bind") cfg.bind_address = next();
    else if (arg == "--port") cfg.port = static_cast<std::uint16_t>(std::stoi(next()));
    else if (arg == "--stream") cfg.stream_id = static_cast<std::uint16_t>(std::stoi(next()));
    else if (arg == "--duration") duration_ms = std::stoull(next());
    else if (arg == "--metrics-out") metrics_path = next();
    else if (arg == "--help") { usage(); return 0; }
    else { std::cerr << "unknown argument: " << arg << "\n"; usage(); return 2; }
  }

  try {
    if (!config_path.empty()) {
      cfg = rudp_rt::load_config_file(config_path, cfg);
    }
    rudp_rt::Gateway gateway(cfg);
    if (!metrics_path.empty()) gateway.set_metrics_path(metrics_path);
    gateway.start();
    const rudp_rt::MetricsSnapshot final_snapshot =
        gateway.run(static_cast<rudp_rt::MonoNs>(duration_ms) *
                    rudp_rt::kNsPerMs);
    // Bounded wait for the metrics worker to persist the final snapshot
    // (INV-19: final snapshot acknowledged before exit).
    for (int i = 0; i < 200 && !gateway.metrics_final_acked(); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout << rudp_rt::format_summary(final_snapshot);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "rudp_gateway: " << e.what() << "\n";
    return 1;
  }
}
