// Controlled GOP failure/recovery, driven through the production protocol nodes.
#include "rudp_rt/sim.hpp"

#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace rudp_rt;

namespace {
struct Event {
  MonoNs time;
  std::uint32_t frame;
  std::string kind;
};
}

int main(int argc, char** argv) {
  try {
    if (argc != 2) {
      std::cerr << "usage: rudp_demo OUTPUT.json\n";
      return 2;
    }
    Config cfg;
    SimCluster cluster(cfg, 0x5EED, 0x5EED ^ 0xA5A5);
    cluster.feed_enabled = false;
    cluster.set_heartbeat(ns_from_ms(1));
    DirectionFaults clean;
    clean.base_delay = ns_from_ms(5);
    cluster.set_gateway_route(clean);
    cluster.add_client(0x5EED + 1000);
    std::vector<Event> events;
    std::set<std::uint32_t> played;
    auto record = [&](std::uint32_t fid, const std::string& kind) {
      events.push_back({cluster.clock.now(), fid, kind});
    };
    cluster.gateway.set_frame_observer([&](std::uint32_t, std::uint32_t fid,
                                           SessionFrameOutcome outcome) {
      record(fid, std::string("sender:") + to_string(outcome));
    });
    cluster.clients[0].node->set_frame_observer([&](std::uint32_t fid,
                                                  SessionFrameOutcome outcome) {
      record(fid, std::string("playout:") + to_string(outcome));
      if (outcome == SessionFrameOutcome::ONTIME) played.insert(fid);
    });
    cluster.gateway.set_keyframe_request_callback([&] { record(0, "keyframe-request"); });
    auto advance = [&](MonoNs duration) {
      const MonoNs end = cluster.clock.now() + duration;
      // SimDriver may return at quiescence; this scripted demo needs exact times.
      while (cluster.clock.now() < end) cluster.run(end);
    };
    advance(ns_from_ms(20));
    if (!cluster.clients[0].node->established()) throw std::runtime_error("join failed");
    auto offer = [&](std::uint32_t fid, FrameType type, std::uint32_t dependency,
                     MonoNs budget) {
      MediaFrame frame;
      frame.stream_id = 1;
      frame.frame_id = fid;
      frame.rtp_timestamp = fid * 3000;
      frame.capture_time = cluster.clock.now();
      frame.deadline_budget = budget;
      frame.frame_type = type;
      frame.depends_on_frame = dependency;
      frame.bytes.assign(12000, std::byte{7});
      record(fid, type == FrameType::KEY_FRAME ? "offer:key" : "offer:delta");
      cluster.gateway.offer_frame(std::move(frame), cluster.clock.now());
    };
    DirectionFaults cut;
    cut.data_loss = 1;
    cut.control_loss = 1;
    cluster.set_gateway_route(cut);
    offer(0, FrameType::KEY_FRAME, 0, ns_from_ms(50));
    offer(1, FrameType::DELTA_FRAME, 0, ns_from_ms(100));
    offer(2, FrameType::DELTA_FRAME, 0, ns_from_ms(100));
    advance(ns_from_ms(60));
    offer(3, FrameType::DELTA_FRAME, 0, ns_from_ms(100));
    cluster.set_gateway_route(clean);
    offer(4, FrameType::KEY_FRAME, 0, ns_from_ms(100));
    offer(5, FrameType::DELTA_FRAME, 4, ns_from_ms(100));
    advance(ns_from_ms(500));
    cluster.shutdown_close();
    const auto ledger = cluster.gateway.aggregate_ledger();
    if (!ledger.conserved() || ledger.in_flight != 0 || ledger.abandoned != 3 ||
        ledger.pre_admission_dropped != 1 || !played.contains(4) || !played.contains(5)) {
      throw std::runtime_error("GOP recovery assertions failed");
    }
    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("cannot write demo output");
    out << "{\"scenario\":\"controlled-gop-recovery\",\"seed\":24301,"
        << "\"clock\":\"virtual\",\"blackout_ms\":[20,80],\"events\":[\n";
    bool first = true;
    for (const auto& e : events) {
      if (!first) out << ",\n";
      first = false;
      out << "{\"time_ms\":" << static_cast<double>(e.time) / 1e6
          << ",\"frame_id\":" << e.frame << ",\"event\":\"" << e.kind << "\"}";
      std::cout << static_cast<double>(e.time) / 1e6 << " ms  frame " << e.frame
                << "  " << e.kind << '\n';
    }
    out << "\n],\"checks\":{\"conserved\":true,\"in_flight\":0,"
        << "\"abandoned\":3,\"pre_admission_dropped\":1,\"recovered_frames\":[4,5]}}\n";
    std::cout << "PASS: three frames abandoned; broken-GOP frame pruned; frames 4/5 played on time\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "rudp_demo: " << error.what() << '\n';
    return 1;
  }
}
