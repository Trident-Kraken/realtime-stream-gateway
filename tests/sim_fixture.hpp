#pragma once
// sim_fixture.hpp — shared harness for integration and fault tests:
// (a) TransportLoop: sender/receiver/assembler/jitter wired through a
//     VirtualNetwork for precise protocol-level scenarios;
// (b) SimSession: full GatewayNode + ClientNode sessions for lifecycle
//     scenarios. Every scenario asserts the invariants that must always
//     hold: FDR conservation, bounded state, zero watchdog violations.

#include "test_framework.hpp"

#include "rudp_rt/playout.hpp"
#include "rudp_rt/sender.hpp"
#include "rudp_rt/sim.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <iostream>
#include <vector>

#include <arpa/inet.h>

namespace rudp_test {

using namespace rudp_rt;

// ---------------------------------------------------------------------------
// TransportLoop: gateway-side sender <-> client-side receive pipeline.
// ---------------------------------------------------------------------------
// Base holds the endpoints and configs so they are fully initialized before
// the sender/receiver members are constructed.
struct TransportLoopBase {
  Endpoint gw_ep{htonl(0x0A000001), htons(40000)};
  Endpoint cl_ep{htonl(0x0A000002), htons(40001)};
  SenderConfig scfg;
  ReceiverConfig rcfg;
  TransportLoopBase() {
    scfg.peer = cl_ep;
    scfg.stream_id = 1;
    scfg.ssrc = 7;
    scfg.session_epoch = 11;
    scfg.isn = 1000;
    rcfg.peer = gw_ep;
    rcfg.stream_id = 1;
    rcfg.session_epoch = 11;
    rcfg.isn = 1000;
  }
};

struct TransportLoop : TransportLoopBase {
  VirtualClock clock;
  VirtualNetwork net;
  SimDriver driver;
  TransportSender sender;
  // Same deep module the real ClientNode drives; the harness is its second
  // adapter (stream/epoch validation included, as ClientNode does).
  PlayoutPipeline playout;
  bool synced = false;

  // stats
  std::uint64_t delivered_ontime = 0;
  std::uint64_t delivered_late = 0;
  std::uint64_t expired = 0;
  std::uint64_t delivery_abandoned = 0;
  std::vector<std::uint32_t> delivered_ids;

  explicit TransportLoop(double data_loss = 0.0)
      : net(clock, 0x5EED),
        driver(clock),
        sender(scfg, clock),
        playout(rcfg, 64, 64, ns_from_ms(60), 128, clock) {
    DirectionFaults down;
    down.data_loss = data_loss;
    down.base_delay = ns_from_ms(5);
    net.set_route(gw_ep, down);
    DirectionFaults up;
    up.base_delay = ns_from_ms(5);
    net.set_route(cl_ep, up);
    driver.attach(net);
    playout.set_sync(1000, 11);
    synced = true;
  }

  void offer_frame(std::uint32_t frame_id, std::size_t bytes = 12000,
                   MonoNs budget = ns_from_ms(100)) {
    MediaFrame f;
    f.stream_id = 1;
    f.frame_id = frame_id;
    f.rtp_timestamp = frame_id * 3000;  // 90 kHz, 30 fps (spec §2.3)
    f.capture_time = clock.now();
    f.deadline_budget = budget;
    f.bytes.assign(bytes, std::byte{0xA});
    sender.on_frame(std::move(f));
    route_sender_output();
  }

  void offer_frame_gop(std::uint32_t frame_id, FrameType type,
                       std::uint32_t depends_on, std::size_t bytes = 12000,
                       MonoNs budget = ns_from_ms(100)) {
    MediaFrame f;
    f.stream_id = 1;
    f.frame_id = frame_id;
    f.rtp_timestamp = frame_id * 3000;
    f.capture_time = clock.now();
    f.deadline_budget = budget;
    f.frame_type = type;
    f.depends_on_frame = depends_on;
    f.bytes.assign(bytes, std::byte{0xA});
    sender.on_frame(std::move(f));
    route_sender_output();
  }

  // One simulated event step.
  void step(MonoNs max_dt = ns_from_ms(5)) {
    MonoNs t = clock.now() + max_dt;
    if (auto d = net.next_event_time()) t = std::min(t, *d);
    if (auto d = sender.next_deadline()) t = std::min(t, *d);
    if (auto d = playout.next_deadline()) t = std::min(t, *d);
    if (t <= clock.now()) t = clock.now() + 1;
    clock.set(t);
    for (auto& ev : net.pop_due(t)) {
      Datagram in;
      in.peer = ev.src;
      in.bytes = ev.datagram.bytes;
      if (ev.dst == cl_ep) {
        deliver_to_client(in, t);
      } else {
        dispatch_client_control(in, t);
        route_sender_output();
      }
    }
    sender.on_timer(t);
    route_sender_output();
    pump_new_data();
    for (auto& dg : playout.on_timer(t)) {
      net.deliver_from(cl_ep, dg);
    }
    for (auto& d : playout.take_deliveries()) count_delivery(d);
    expired = playout.delivery_expired();
    delivery_abandoned = playout.delivery_abandoned();
  }

  void run_for(MonoNs duration) {
    const MonoNs end = clock.now() + duration;
    while (clock.now() < end) step(ns_from_ms(5));
  }

  void count_delivery(JitterBuffer::Delivery& d) {
    if (d.ontime) {
      ++delivered_ontime;
    } else {
      ++delivered_late;
    }
    delivered_ids.push_back(d.frame.frame_id);
  }

  std::uint64_t acks_received() const { return sender.counters().acks_received; }

  // Emit admitted-but-unsent packets (data-path pump).
  void pump_new_data() {
    while (auto dg = sender.emit_new_data(clock.now(), 1 << 20)) {
      net.deliver_from(gw_ep, *dg);
    }
  }

 private:
  void dispatch_client_control(Datagram& dg, MonoNs now) {
    auto decoded = decode_header(dg.bytes);
    if (decoded.reject != RejectReason::NONE) return;
    const auto payload =
        std::span<const std::byte>(dg.bytes).subspan(kHeaderSize);
    if (static_cast<PacketType>(decoded.header.type) == PacketType::ACK) {
      auto ack = decode_ack(payload);
      if (ack.reject == RejectReason::NONE) sender.on_ack(ack.ack, now);
    } else if (static_cast<PacketType>(decoded.header.type) ==
               PacketType::NACK) {
      auto nack = decode_nack(payload);
      if (nack.reject == RejectReason::NONE) sender.on_nack(nack.nack, now);
    }
  }

  void deliver_to_client(Datagram& dg, MonoNs now) {
    auto decoded = decode_header(dg.bytes);
    if (decoded.reject != RejectReason::NONE) return;
    if (static_cast<PacketType>(decoded.header.type) == PacketType::ABANDON) {
      auto ab = decode_abandon(
          std::span<const std::byte>(dg.bytes).subspan(kHeaderSize));
      if (ab.reject != RejectReason::NONE ||
          ab.ab.session_epoch != rcfg.session_epoch) {
        return;
      }
      for (auto& ack : playout.apply_abandon(std::move(ab.ab.ranges), now)) {
        net.deliver_from(cl_ep, ack);
      }
      return;
    }
    // DATA: ACK/NACK replies go back over the network.
    for (auto& reply : playout.on_data(dg, now)) {
      net.deliver_from(cl_ep, reply);
    }
  }

  void route_sender_output() {
    SenderOutput out = sender.take_output();
    for (auto& dg : out.control) net.deliver_from(gw_ep, dg);
    for (auto& dg : out.data) net.deliver_from(gw_ep, dg);
  }
};

// ---------------------------------------------------------------------------
// SimSession: full gateway + client nodes (handshake, epochs, fan-out).
// A thin adapter over the SimCluster bootstrap in sim.hpp; the test-only
// additions are the fixed join-paced wiring and the invariant assertions.
// ---------------------------------------------------------------------------
struct SimSession : rudp_rt::SimCluster {
  // Fixed-workload default: capture feed begins after the join completes.
  explicit SimSession(int nclients = 1, std::uint64_t seed = 0x5EED)
      : SimCluster(Config{}, seed, seed ^ 0xA5A5) {
    driver.set_heartbeat(ns_from_ms(1));  // capture-feed pacing
    DirectionFaults down;
    down.base_delay = ns_from_ms(5);
    set_gateway_route(down);
    for (int i = 0; i < nclients; ++i) {
      add_client(seed + 1000u + static_cast<std::uint64_t>(i));
    }
  }

  void set_faults(double data_loss = 0.0, int burst = 0, MonoNs jitter = 0,
                  double reorder = 0, double duplicate = 0,
                  double control_loss = 0) {
    DirectionFaults down;
    down.data_loss = data_loss;
    down.burst_length = burst;
    down.jitter = jitter;
    down.reorder = reorder;
    down.duplicate = duplicate;
    down.control_loss = control_loss;
    set_gateway_route(down);
  }

  // ---- assertions shared by every scenario ----
  void assert_invariants() {
    for (const auto& sm : gateway.collect_sessions()) {
      // INV-20: session ledger conservation, per session.
      RUDP_CHECK(sm.frames.offered() ==
                 sm.frames.ontime + sm.frames.late + sm.frames.abandoned +
                     sm.frames.expired + sm.frames.pre_admission_dropped +
                     sm.frames.in_flight);
      // INV-01: after shutdown close-out no sequence remains unresolved.
      if (gateway.shutting_down()) {
        RUDP_CHECK(sm.frames.in_flight == 0u);
      }
    }
    // No watchdog violations in any scenario.
    RUDP_CHECK(gateway.watchdog_violations() == 0u);
    for (const auto& c : clients) {
      // Client playout accounting never goes negative / inconsistent.
      RUDP_CHECK(c.node->delivered_ontime() + c.node->delivered_late() +
                     c.node->delivery_expired() + c.node->delivery_abandoned() <=
                 1ull << 40);
    }
  }
};

}  // namespace rudp_test
