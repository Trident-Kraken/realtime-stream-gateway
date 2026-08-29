#pragma once
// rudp_rt/sim.hpp — deterministic in-process virtual clock / virtual network
// harness (spec §0.1 #4, §5 "rudp_sim"). Virtual and real networks implement
// the same INetwork interface; the driver drives GatewayNode/ClientNode
// through the same event API as the Linux Reactor, so protocol behavior is
// identical up to OS scheduling.
//
// Determinism contract: same seed + same scenario => identical event order,
// identical metrics. The RNG is consulted only from the driver thread in a
// fixed order (send-time fault decisions in pump order).

#include "rudp_rt/clock.hpp"
#include "rudp_rt/media.hpp"
#include "rudp_rt/network.hpp"
#include "rudp_rt/session.hpp"
#include "rudp_rt/types.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <queue>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <random>
#include <arpa/inet.h>
#include <vector>

namespace rudp_rt {

// Per-direction fault configuration. "Direction" = the sending endpoint.
// DATA-path and control-path losses are configured separately (spec §8.2:
// "DATA-path loss unless a case says control-path loss"); the path is
// derived from the datagram type byte inside the virtual network.
struct DirectionFaults {
  double data_loss = 0.0;        // IID loss probability
  double control_loss = 0.0;
  int burst_length = 0;          // >0: burst model with this fixed length
  MonoNs base_delay = 0;         // one-way
  MonoNs jitter = 0;             // uniform in [-jitter, +jitter]
  double reorder = 0.0;          // probability of an overtaking delay
  double duplicate = 0.0;        // probability of an extra copy
};

struct DeliveryEvent {
  MonoNs time = 0;
  std::uint64_t seq = 0;
  Endpoint src;  // original sender (becomes Datagram::peer on receive)
  Endpoint dst;
  Datagram datagram;  // datagram.peer = destination (send convention)
};

class VirtualNetwork {
 public:
  VirtualNetwork(IClock& clock, std::uint64_t seed)
      : clock_(clock), rng_(seed) {}

  void set_route(const Endpoint& from, const DirectionFaults& faults) {
    FaultConfig& fc = routes_[from];
    static_cast<DirectionFaults&>(fc) = faults;
    fc.burst_left_ = std::make_shared<int>(0);
  }

  const DirectionFaults* route(const Endpoint& from) const {
    const auto it = routes_.find(from);
    return it == routes_.end() ? nullptr : &it->second;
  }

  // Schedules delivery of one datagram after applying the direction faults.
  void deliver_from(const Endpoint& src, const Datagram& dg) {
    if (!drop_next_.empty() && dg.bytes.size() > 1) {
      const auto type = static_cast<PacketType>(dg.bytes[1]);
      auto it = drop_next_.find(type);
      if (it != drop_next_.end() && it->second > 0) {
        --(it->second);
        stats_.dropped++;
        return;
      }
    }
    const auto fit = routes_.find(src);
    if (fit == routes_.end()) {
      schedule(0, src, dg, 0);
      return;
    }
    const FaultConfig& fc = fit->second;
    const DirectionFaults* f = &fc;
    const bool is_control =
        dg.bytes.size() > 1 &&
        static_cast<std::uint8_t>(dg.bytes[1]) !=
            static_cast<std::uint8_t>(PacketType::DATA);
    const double loss = is_control ? f->control_loss : f->data_loss;

    // Burst model: fixed-length bursts; the configured probability is the
    // TARGET AVERAGE loss rate, so the burst-start probability is
    // h = L / (len * (1 - L)) (Gilbert-type normalization; ADR-0012).
    if (!is_control && fc.burst_length > 1 && fc.data_loss > 0) {
      if (*fc.burst_left_ > 0) {
        (*fc.burst_left_)--;
        stats_.dropped++;
        return;
      }
      const double L = std::min(fc.data_loss, 0.99);
      const double h = L / (static_cast<double>(fc.burst_length) * (1.0 - L));
      if (std::uniform_real_distribution<>(0.0, 1.0)(rng_) < h) {
        *fc.burst_left_ = fc.burst_length - 1;
        stats_.dropped++;
        return;
      }
    } else if (loss > 0) {
      if (std::uniform_real_distribution<>(0.0, 1.0)(rng_) < loss) {
        stats_.dropped++;
        return;
      }
    }

    MonoNs delay = f->base_delay;
    if (f->jitter > 0) {
      std::uniform_int_distribution<MonoNs> d(-f->jitter, f->jitter);
      delay = std::max<MonoNs>(0, delay + d(rng_));
    }
    if (f->reorder > 0 &&
        std::uniform_real_distribution<>(0.0, 1.0)(rng_) < f->reorder) {
      std::uniform_int_distribution<MonoNs> d(0, std::max<MonoNs>(1, f->base_delay + f->jitter));
      delay = d(rng_);  // overtake: earlier than the normal path
    }

    schedule(delay, src, dg, 0);
    if (f->duplicate > 0 &&
        std::uniform_real_distribution<>(0.0, 1.0)(rng_) < f->duplicate) {
      schedule(delay + 500 * kNsPerUs, src, dg, 0);  // extra copy
      stats_.duplicated++;
    }
  }

  void schedule(MonoNs delay, const Endpoint& src, const Datagram& dg,
                std::uint64_t) {
    DeliveryEvent e;
    e.time = clock_.now() + delay;
    e.seq = ++seq_counter_;
    e.src = src;
    e.dst = dg.peer;
    e.datagram = dg;
    events_.push(std::move(e));
  }

  std::optional<MonoNs> next_event_time() const {
    if (events_.empty()) return std::nullopt;
    return events_.top().time;
  }

  // Pops all events due at or before `now` in (time, seq) order.
  std::vector<DeliveryEvent> pop_due(MonoNs now) {
    std::vector<DeliveryEvent> out;
    while (!events_.empty() && events_.top().time <= now) {
      out.push_back(events_.top());
      events_.pop();
    }
    return out;
  }

  std::size_t in_flight() const { return events_.size(); }

  struct Stats {
    std::uint64_t dropped = 0;
    std::uint64_t duplicated = 0;
    std::uint64_t delivered = 0;
  };
  const Stats& stats() const { return stats_; }

  // Fault-injection hooks for targeted scenarios (lost SYNC_START, lost
  // ABANDON, ...): drop the next N datagrams of one type once.
  void arm_drop_next(PacketType type, int n) { drop_next_[type] = n; }
  // Drop every datagram of the type for the rest of the run.
  void arm_drop_all(PacketType type) {
    drop_next_[type] = std::numeric_limits<int>::max();
  }

 private:
  struct FaultConfig : DirectionFaults {
    std::shared_ptr<int> burst_left_ = std::make_shared<int>(0);
  };

  struct EventGreater {
    bool operator()(const DeliveryEvent& a, const DeliveryEvent& b) const {
      if (a.time != b.time) return a.time > b.time;
      return a.seq > b.seq;
    }
  };

  IClock& clock_;
  std::mt19937_64 rng_;
  std::map<Endpoint, FaultConfig> routes_;
  std::priority_queue<DeliveryEvent, std::vector<DeliveryEvent>,
                      EventGreater>
      events_;
  std::uint64_t seq_counter_ = 0;
  std::map<PacketType, int> drop_next_;
  Stats stats_;
};

// ---------------------------------------------------------------------------
// VirtualSocket — INetwork adapter bound to one local endpoint.
// ---------------------------------------------------------------------------
class VirtualSocket final : public INetwork {
 public:
  VirtualSocket(VirtualNetwork& net, Endpoint local) : net_(net), local_(local) {}

  SendResult send(const Endpoint& peer,
                  std::span<const std::byte> bytes) override {
    if (would_block_ > 0) {
      --would_block_;  // deterministic pending-datagram injection (ADR-0009)
      return {SendStatus::WOULD_BLOCK};
    }
    Datagram dg;
    dg.peer = peer;
    dg.bytes.assign(bytes.begin(), bytes.end());
    net_.deliver_from(local_, dg);
    net_.stats();  // delivered counted by the driver
    return {SendStatus::SENT};
  }

  RecvResult receive(std::span<std::byte>) override {
    return {RecvStatus::WOULD_BLOCK, {}, 0};  // deliveries dispatch directly
  }

  Endpoint local() const { return local_; }

  // The next `count` send() calls return WOULD_BLOCK once each; the node's
  // pending_out slot is retried on the next pump, so the ADR-0009
  // pending-datagram path is exercised under simulation too.
  void arm_would_block(int count) { would_block_ = count; }

 private:
  VirtualNetwork& net_;
  Endpoint local_;
  int would_block_ = 0;
};

// ---------------------------------------------------------------------------
// SimDriver — the virtual event loop (delivery, timers, pumps in fixed
// order). Gateway is always processed before clients for determinism.
// ---------------------------------------------------------------------------
class SimDriver {
 public:
  explicit SimDriver(VirtualClock& clock) : clock_(clock) {}

  struct Participant {
    GatewayNode* gateway = nullptr;
    ClientNode* client = nullptr;
    VirtualSocket* socket = nullptr;
    int index = 0;
  };

  void add_gateway(GatewayNode& g, VirtualSocket& s) {
    participants_.push_back({&g, nullptr, &s, -1});
  }
  void add_client(ClientNode& c, VirtualSocket& s, int index) {
    participants_.push_back({nullptr, &c, &s, index});
  }

  // Runs until `until` or quiescence; calls `tick` after each event batch.
  void run(MonoNs until, const std::function<void(SimDriver&)>& tick = nullptr) {
    MonoNs last_t = -1;
    while (clock_.now() < until) {
      MonoNs t = until;
      if (heartbeat_ > 0) t = std::min(t, clock_.now() + heartbeat_);
      if (auto d = net_->next_event_time()) t = std::min(t, *d);
      for (const auto& p : participants_) {
        if (p.gateway) {
          if (auto d = p.gateway->next_deadline()) t = std::min(t, *d);
        } else if (p.client) {
          if (auto d = p.client->next_deadline()) t = std::min(t, *d);
        }
      }
      if (t <= last_t) {
        // Guarantee progress; log repeated forced steps (livelock detector).
        if (++forced_steps_ > 1000) {
          if (forced_steps_ > 1100) {
            std::fprintf(stderr, "SIM LIVELOCK: forced steps at t=%lld\n",
                         (long long)last_t);
            for (const auto& p : participants_) {
              if (p.gateway) {
                std::fprintf(stderr, "  gw deadline=%lld\n",
                             (long long)(p.gateway->next_deadline()
                                             ? *p.gateway->next_deadline()
                                             : -1));
              } else if (p.client) {
                std::fprintf(stderr, "  client deadline=%lld\n",
                             (long long)(p.client->next_deadline()
                                             ? *p.client->next_deadline()
                                             : -1));
              }
            }
            std::abort();
          }
        }
        t = last_t + 1;
      } else {
        forced_steps_ = 0;
      }
      last_t = t;
      clock_.set(t);

      // 1) Deliveries in scheduled order.
      for (auto& ev : net_->pop_due(t)) {
        net_stats_.delivered++;
        dispatch(ev, t);
      }
      // 2) Timers (gateway first, then clients in registration order).
      for (const auto& p : participants_) {
        if (p.gateway) {
          if (auto d = p.gateway->next_deadline(); d && *d <= t) {
            p.gateway->on_timer(t);
          }
        } else if (p.client) {
          if (auto d = p.client->next_deadline(); d && *d <= t) {
            p.client->on_timer(t);
          }
        }
      }
      // 3) Pumps (same order).
      for (const auto& p : participants_) {
        if (p.gateway) {
          p.gateway->pump(*p.socket, t);
        } else if (p.client) {
          p.client->pump(*p.socket, t);
        }
      }
      if (tick) tick(*this);
      if (quiescent(until)) break;
    }
  }

  void attach(VirtualNetwork& net) { net_ = &net; }

  // Periodic heartbeat emulating the capture worker's pacing thread; the
  // tick callback fires at least once per interval of simulated time.
  void set_heartbeat(MonoNs interval) { heartbeat_ = interval; }

  bool quiescent(MonoNs until) const {
    if (net_->in_flight() > 0) return false;
    for (const auto& p : participants_) {
      if (p.gateway) {
        if (auto d = p.gateway->next_deadline(); d && *d < until) return false;
      } else if (p.client) {
        if (auto d = p.client->next_deadline(); d && *d < until) return false;
      }
    }
    return true;
  }

  const VirtualNetwork::Stats& net_stats() const { return net_stats_; }

 private:
  void dispatch(const DeliveryEvent& ev, MonoNs now) {
    for (const auto& p : participants_) {
      if (p.socket->local() == ev.dst) {
        // Receive convention: Datagram::peer is the remote sender.
        Datagram incoming;
        incoming.peer = ev.src;
        incoming.bytes = ev.datagram.bytes;
        if (p.gateway) p.gateway->on_datagram(incoming, now);
        else if (p.client) p.client->on_datagram(incoming, now);
        return;
      }
    }
    unmatched_++;
  }

  VirtualClock& clock_;
  VirtualNetwork* net_ = nullptr;
  std::vector<Participant> participants_;
  VirtualNetwork::Stats net_stats_;
  std::uint64_t unmatched_ = 0;
  long forced_steps_ = 0;
  MonoNs heartbeat_ = 0;

 public:
  std::uint64_t unmatched() const { return unmatched_; }
};

// ---------------------------------------------------------------------------
// SimCluster — one deterministic virtual fan-out cluster: gateway + clients
// + virtual network + capture feed, driven by a SimDriver. The single
// bootstrap behind rudp_sim, the rudp_bench RUDP arms, and the integration/
// fault/property harnesses; the wiring used to live in three hand-rolled
// copies.
//
// `seed` drives the virtual network and every client rng (seed + 1000 +
// index); `gateway_seed` derives the gateway's epoch/ISN rng so callers keep
// their recorded mixes. Capture feed: every frame the source produces is
// offered to the gateway while feed_enabled; `fed` counts them.
// ---------------------------------------------------------------------------
class SimCluster {
 public:
  struct ClientHandle {
    std::unique_ptr<ClientNode> node;
    std::unique_ptr<VirtualSocket> sock;
    Endpoint ep;
  };

  // Fixed-workload source shape shared by the bench arms and the test
  // harness: 12,000 B frames at 30 fps, capture beginning at 20 ms.
  static SyntheticSource::Params default_source_params() {
    SyntheticSource::Params sp;
    sp.stream_id = 1;
    sp.media_bytes = 12000;
    sp.fps = 30;
    sp.start_time = ns_from_ms(20);
    return sp;
  }

  SimCluster(const Config& cfg, std::uint64_t seed, std::uint64_t gateway_seed,
             SyntheticSource::Params sp = default_source_params(),
             Endpoint gateway_ep = Endpoint{htonl(0x0A000001), htons(50000)})
        : cfg_(cfg),
          net(clock, seed),
          driver(clock),
          source(sp),
          gw_ep(gateway_ep),
          // Nodes hold const Config& — bind them to our own cfg_ member
          // (declared first), never to the caller's argument.
          gateway(cfg_, clock,
                  [engine = std::mt19937_64(gateway_seed)]() mutable {
                    return static_cast<std::uint32_t>(engine());
                  }),
          gw_sock(net, gw_ep) {
    driver.attach(net);
    driver.add_gateway(gateway, gw_sock);
  }

  // Wiring (before the first run; routes may be re-set between runs).
  ClientHandle& add_client(std::uint64_t client_seed) {
    const auto ep = Endpoint{
        htonl(0x0A000002u + static_cast<std::uint32_t>(clients.size())),
        htons(static_cast<std::uint16_t>(20000 + clients.size()))};
    DirectionFaults up;
    up.base_delay = ns_from_ms(5);
    net.set_route(ep, up);
    auto node = std::make_unique<ClientNode>(
        cfg_, clock, gw_ep, cfg_.stream_id,
        [engine = std::mt19937_64(client_seed)]() mutable {
          return static_cast<std::uint32_t>(engine());
        });
    auto sock = std::make_unique<VirtualSocket>(net, ep);
    node->start_join(0);
    driver.add_client(*node, *sock, static_cast<int>(clients.size()));
    clients.push_back({std::move(node), std::move(sock), ep});
    return clients.back();
  }
  void set_gateway_route(const DirectionFaults& f) { net.set_route(gw_ep, f); }
  void set_client_route(std::size_t index, const DirectionFaults& f) {
    net.set_route(clients[index].ep, f);
  }
  void set_heartbeat(MonoNs interval) { driver.set_heartbeat(interval); }
  void enable_latencies() {
    for (auto& c : clients) c.node->record_latencies(true);
  }

  // Run until `end` on the virtual clock; `hook` fires after each event
  // batch, after the capture feed.
  void run(MonoNs end, const std::function<void(SimCluster&)>& hook = nullptr) {
    driver.run(end, [&](SimDriver&) {
      if (feed_enabled) {
        const MonoNs now = clock.now();
        while (auto f = source.next(now)) {
          ++fed;
          gateway.offer_frame(std::move(*f), now);
        }
      }
      if (hook) hook(*this);
    });
  }
  void run_for(MonoNs duration,
               const std::function<void(SimCluster&)>& hook = nullptr) {
    run(clock.now() + duration, hook);
  }
  // Shutdown barrier + ABANDON flush, like the runtime close-out (INV-18).
  void shutdown_close(MonoNs flush = ns_from_ms(50)) {
    gateway.begin_shutdown(clock.now());
    run_for(flush);
  }

  // --- members (direct access is the module's interface) ---
  Config cfg_;
  VirtualClock clock;
  VirtualNetwork net;
  SimDriver driver;
  SyntheticSource source;
  Endpoint gw_ep;
  GatewayNode gateway;
  VirtualSocket gw_sock;
  std::vector<ClientHandle> clients;
  bool feed_enabled = true;
  std::uint64_t fed = 0;
};

}  // namespace rudp_rt
