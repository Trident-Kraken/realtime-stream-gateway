// bench_main.cpp — rudp_bench: reproducible benchmark runner for the three
// comparison experiments (spec §8.2 fixed workload; ADR-0012 arms).
//
//   1. UDP vs TCP-model vs RUDP-RT under the same seeded lossy network.
//   2. Deadline-aware+ABANDON vs always-retransmit (same RTO configuration).
//   3. Fan-out to three clients (session-ledger conservation per client).
//
// Fixed workload (spec §8.2): 30 fps, 12,000 media bytes/frame, 10 DATA
// packets/frame, 10 ms base RTT, ±2 ms jitter, 100 ms sender deadline,
// 60 ms playout delay, 100-frame warm-up then 10,000 measured frames,
// seed 0x5EED (plus 0x5EED+1..4 for the confidence interval).

#include "rudp_rt/sim.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <vector>

#include <unistd.h>

#include <arpa/inet.h>
#include <sys/time.h>
#include <cstdlib>

using namespace rudp_rt;

namespace {

constexpr std::uint64_t kBaseSeed = 0x5EED;
constexpr std::uint32_t kWarmupFrames = 100;
std::uint32_t kMeasuredFrames = 10000;  // overridable via --frames
constexpr int kRtpIncrement = 3000;  // 90 kHz at 30 fps

// ---------------------------------------------------------------------------
// Result record for one arm run
// ---------------------------------------------------------------------------
struct BenchResult {
  std::string arm;
  std::uint64_t seed = 0;
  double loss = 0;
  int burst = 0;
  int clients = 1;
  std::uint64_t ontime = 0;
  std::uint64_t late = 0;
  std::uint64_t expired = 0;
  std::uint64_t abandoned = 0;
  std::uint64_t offered = 0;
  std::uint64_t net_dropped = 0;
  double fdr_session = 0;
  double fdr_playout = 0;
  double p50_ms = 0, p95_ms = 0, p99_ms = 0, max_ms = 0;
  double cpu_s = 0;
  long mem_hwm_kb = 0;
  double wall_s = 0;
};

double percentile(std::vector<MonoNs> v, double p) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  const std::size_t idx = static_cast<std::size_t>(
      p * static_cast<double>(v.size() - 1));
  return static_cast<double>(v[idx]) / 1e6;
}

rusage now_rusage() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  return ru;
}
double cpu_seconds(const rusage& ru) {
  return static_cast<double>(ru.ru_utime.tv_sec) +
         static_cast<double>(ru.ru_utime.tv_usec) / 1e6 +
         static_cast<double>(ru.ru_stime.tv_sec) +
         static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
}

// ---------------------------------------------------------------------------
// RUDP arm: the real protocol nodes over the virtual network
// ---------------------------------------------------------------------------
BenchResult run_rudp_arm(const std::string& arm, std::uint64_t seed,
                         double loss, int burst, int nclients,
                         bool deadline_mode) {
  BenchResult r;
  r.arm = arm;
  r.seed = seed;
  r.loss = loss;
  r.burst = burst;
  r.clients = nclients;

  const rusage ru0 = now_rusage();
  const double wall0 = static_cast<double>(clock()) / CLOCKS_PER_SEC;
  (void)wall0;

  Config cfg;
  cfg.deadline_mode = deadline_mode;
  cfg.max_frame_packets = 10;
  cfg.frame_bytes = 12000;
  cfg.media_payload_bytes = 1200;

  SimCluster cluster(cfg, seed, seed ^ 0xA5A5A5A5u);

  DirectionFaults down;
  down.data_loss = loss;
  down.burst_length = burst;
  down.base_delay = ns_from_ms(5);  // 10 ms RTT
  down.jitter = ns_from_ms(2);      // ±2 ms
  cluster.set_gateway_route(down);
  DirectionFaults up;
  up.base_delay = ns_from_ms(5);
  up.jitter = ns_from_ms(2);
  for (int i = 0; i < nclients; ++i) {
    cluster.add_client(seed + 1000u + static_cast<std::uint64_t>(i));
    cluster.set_client_route(static_cast<std::size_t>(i), up);
  }
  cluster.enable_latencies();
  cluster.set_heartbeat(ns_from_ms(1));

  const MonoNs sim_duration =
      static_cast<MonoNs>(static_cast<std::uint64_t>(kWarmupFrames +
                                                     kMeasuredFrames + 400) *
                              1000 * kNsPerMs / 30);
  cluster.run_for(sim_duration);

  cluster.gateway.begin_shutdown(cluster.clock.now());
  cluster.run_for(ns_from_ms(100));

  // Aggregate client playout measurement over the measured window.
  for (auto& c : cluster.clients) {
    r.ontime += c.node->delivered_ontime();
    r.late += c.node->delivered_late();
    r.expired += c.node->delivery_expired();
    r.abandoned += c.node->delivery_abandoned();
  }
  for (const auto& sm : cluster.gateway.collect_sessions()) {
    r.offered += sm.frames.offered();
  }
  std::uint64_t ontime_total = 0;
  for (const auto& sm : cluster.gateway.collect_sessions()) {
    ontime_total += sm.frames.ontime;
  }
  r.fdr_session =
      r.offered > 0
          ? static_cast<double>(ontime_total) / static_cast<double>(r.offered)
          : 0.0;
  const std::uint64_t playout_den =
      r.ontime + r.late + r.expired + r.abandoned;
  r.fdr_playout = playout_den > 0
                      ? static_cast<double>(r.ontime) /
                            static_cast<double>(playout_den)
                      : 0.0;
  r.net_dropped = cluster.net.stats().dropped;

  // Latency percentiles for measured frames (frame_id >= warmup), client 0.
  std::vector<MonoNs> latencies;
  const auto& node0 = *cluster.clients[0].node;
  const auto& lat = node0.latencies();
  const auto& ids = node0.delivered_frame_ids();
  for (std::size_t i = 0; i < lat.size(); ++i) {
    if (ids[i] >= kWarmupFrames) latencies.push_back(lat[i]);
  }
  r.p50_ms = percentile(latencies, 0.50);
  r.p95_ms = percentile(latencies, 0.95);
  r.p99_ms = percentile(latencies, 0.99);
  r.max_ms = percentile(latencies, 1.0);

  const rusage ru1 = now_rusage();
  r.cpu_s = cpu_seconds(ru1) - cpu_seconds(ru0);
  r.mem_hwm_kb = static_cast<long>(ru1.ru_maxrss);
  r.wall_s = r.cpu_s;  // single-threaded sim: CPU approximates wall time
  return r;
}

// ---------------------------------------------------------------------------
// UDP arm: fire-and-forget fragments; frames complete only when every
// fragment arrives; playout classification identical to the client.
// ---------------------------------------------------------------------------
struct UdpArmReceiver {
  std::map<std::uint32_t, std::pair<std::uint32_t, std::uint32_t>> pending;
  std::uint64_t ontime = 0, late = 0, expired = 0;
  std::uint64_t delivered_frames = 0;
  MonoNs anchor_arrival = -1;
  std::uint32_t anchor_rtp = 0;

  MonoNs playout(std::uint32_t rtp_ts) const {
    const std::int32_t d = static_cast<std::int32_t>(rtp_ts - anchor_rtp);
    return anchor_arrival +
           static_cast<MonoNs>(
               (static_cast<std::int64_t>(d) * kNsPerSec) / kRtpClockRate) +
           ns_from_ms(60);
  }
};

BenchResult run_udp_arm(std::uint64_t seed, double loss, int burst) {
  BenchResult r;
  r.arm = "udp";
  r.seed = seed;
  r.loss = loss;
  r.burst = burst;

  const rusage ru0 = now_rusage();
  Config cfg;

  VirtualClock clock;
  VirtualNetwork net(clock, seed);
  const auto gw_ep = Endpoint{htonl(0x0A000001), htons(50000)};
  const auto cl_ep = Endpoint{htonl(0x0A000002), htons(20000)};
  DirectionFaults down;
  down.data_loss = loss;
  down.burst_length = burst;
  down.base_delay = ns_from_ms(5);
  down.jitter = ns_from_ms(2);
  net.set_route(gw_ep, down);
  DirectionFaults up;
  up.base_delay = ns_from_ms(5);
  net.set_route(cl_ep, up);
  VirtualSocket gw_sock(net, gw_ep);

  Packetizer pz;
  SyntheticSource::Params sp;
  sp.stream_id = 1;
  sp.media_bytes = 12000;
  sp.fps = 30;
  sp.start_time = 0;
  SyntheticSource source(sp);

  UdpArmReceiver rx;
  std::uint64_t offered = 0, sent = 0;
  std::uint32_t next_frame = 0;
  MonoNs next_expiry_check = 0;

  // Simple event loop: feed frames, send fragments once, deliver arrivals.
  while (next_frame < kWarmupFrames + kMeasuredFrames + 200) {
    MonoNs t = clock.now() + ns_from_ms(1);
    if (auto d = net.next_event_time()) t = std::min(t, *d);
    if (t <= clock.now()) t = clock.now() + 1;
    clock.set(t);

    for (auto& ev : net.pop_due(t)) {
      const auto payload = std::span<const std::byte>(ev.datagram.bytes);
      auto hdr = decode_header(payload);
      if (hdr.reject != RejectReason::NONE) continue;
      const std::uint32_t fid = hdr.header.frame_id;
      if (rx.anchor_arrival < 0) {
        rx.anchor_arrival = t;
        rx.anchor_rtp = hdr.header.rtp_timestamp;
      }
      auto& st = rx.pending[fid];
      st.first |= 1u << hdr.header.frame_index;
      ++st.second;
      if (st.second == hdr.header.frame_count) {
        const MonoNs playout = rx.playout(hdr.header.rtp_timestamp);
        if (t <= playout + kNsPerMs) ++rx.ontime; else ++rx.late;
        rx.pending.erase(fid);
      }
    }

    while (auto f = source.next(t)) {
      const bool measured = next_frame >= kWarmupFrames;
      auto pf = pz.packetize(std::move(*f), 1, 1200, 10);
      if (measured) ++offered;
      if (pf) {
        for (const auto& frag : pf->fragments) {
          Header h;
          h.version = 1;
          h.header_len = 7;
          h.type = static_cast<std::uint8_t>(PacketType::DATA);
          h.flags = frag.frame_end ? kFlagFrameEnd : 0;
          h.stream_id = 1;
          h.frame_id = pf->frame_id;
          h.rtp_timestamp = pf->rtp_timestamp;
          h.frame_index = frag.index;
          h.frame_count = pf->frame_count();
          RtpHeader rtp;
          rtp.timestamp = pf->rtp_timestamp;
          rtp.marker = frag.frame_end;
          auto dg = build_data_datagram(cl_ep, h, rtp,
                                        std::span<const std::byte>(
                                            pf->bytes.data() + frag.offset,
                                            frag.len));
          if (dg) {
            net.deliver_from(gw_ep, *dg);
            if (measured) ++sent;
          }
        }
      }
      ++next_frame;
    }

    // Expiry: frames incomplete at playout are expired.
    if (t >= next_expiry_check) {
      next_expiry_check = t + ns_from_ms(5);
      std::vector<std::uint32_t> gone;
      for (auto& [fid, st] : rx.pending) {
        const std::uint32_t rtp = fid * kRtpIncrement;
        if (rx.playout(rtp) + kNsPerMs <= t) {
          ++rx.expired;
          gone.push_back(fid);
        }
      }
      for (const auto fid : gone) rx.pending.erase(fid);
    }
  }

  r.offered = offered;
  r.ontime = rx.ontime;
  r.late = rx.late;
  r.expired = rx.expired;
  r.net_dropped = net.stats().dropped;
  const std::uint64_t den = r.ontime + r.late + r.expired;
  r.fdr_playout =
      den ? static_cast<double>(r.ontime) / static_cast<double>(den) : 0.0;
  r.fdr_session = r.fdr_playout;
  const rusage ru1 = now_rusage();
  r.cpu_s = cpu_seconds(ru1) - cpu_seconds(ru0);
  r.mem_hwm_kb = static_cast<long>(ru1.ru_maxrss);
  return r;
}

// ---------------------------------------------------------------------------
// TCP-model arm: ordered reliable byte stream with RTO retransmission and
// strict in-order delivery (ADR-0012). Frames complete only when every
// preceding byte has been delivered — the head-of-line-blocking property
// that distinguishes TCP for this workload.
// ---------------------------------------------------------------------------
struct TcpModelArm {
  // TCP-Reno-style model (ADR-0012): receiver buffers out-of-order segments
  // (window 64), duplicate-ACK fast retransmit, RTO with per-episode backoff.
  static constexpr std::uint32_t kSegBytes = 1200;
  static constexpr MonoNs kTcpRtoInit = ns_from_ms(200);
  static constexpr std::uint32_t kWindowSegs = 64;
  static constexpr int kDupAckThreshold = 3;

  VirtualClock* clock_ptr = nullptr;
  VirtualNetwork* net_ptr = nullptr;
  Endpoint src_ep;
  Endpoint dst;

  // sender
  std::uint32_t send_una = 0;
  std::uint32_t send_next = 0;
  MonoNs head_last_send = -1;
  std::uint32_t head_retx = 0;
  MonoNs rto = kTcpRtoInit;
  std::uint32_t last_acked = 0;   // for duplicate-ACK detection
  bool has_ack = false;           // the first cumulative ACK is progress
  int dup_acks = 0;
  bool fast_retx_inflight = false;
  std::uint64_t retransmissions = 0;
  std::uint64_t sent = 0;
  bool payload_done = false;

  // receiver
  std::uint32_t rcv_next = 0;
  std::set<std::uint32_t> buffered;
  std::uint64_t ontime = 0, late = 0, expired = 0;
  std::uint64_t delivered_frames = 0;
  MonoNs anchor_arrival = -1;
  std::uint32_t anchor_rtp = 0;

  MonoNs playout(std::uint32_t rtp_ts) const {
    const std::int32_t d = static_cast<std::int32_t>(rtp_ts - anchor_rtp);
    return anchor_arrival +
           static_cast<MonoNs>(
               (static_cast<std::int64_t>(d) * kNsPerSec) / kRtpClockRate) +
           ns_from_ms(60);
  }

  MonoNs now() const { return clock_ptr->now(); }

  // Sends produced-but-unACKed segments inside the window. `payload_done`
  // stops the PRODUCER, never the (re)transmission of produced segments.
  void send_window(MonoNs now, std::uint32_t produced_segs) {
    while (send_next < send_una + kWindowSegs && send_next < produced_segs) {
      auto dg = make_seg(send_next, now, false);
      net_ptr->deliver_from(src_ep, *dg);
      if (head_last_send < 0) head_last_send = now;
      ++send_next;
      ++sent;
    }
  }

  std::optional<Datagram> make_seg(std::uint32_t seq, MonoNs, bool) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::DATA);
    h.seq = seq;
    h.stream_id = 1;
    RtpHeader rtp;
    return build_data_datagram(dst, h, rtp,
                               std::vector<std::byte>(kSegBytes, std::byte{7}));
  }

  void retransmit_head(MonoNs now) {
    if (send_una >= send_next) return;
    auto dg = make_seg(send_una, now, true);
    if (dg) net_ptr->deliver_from(src_ep, *dg);
    head_last_send = now;
    ++head_retx;
    ++retransmissions;
  }

  void on_timer(MonoNs now) {
    if (send_next > send_una && head_last_send >= 0 &&
        now - head_last_send >= rto) {
      retransmit_head(now);
      rto = std::min<MonoNs>(rto * 2, ns_from_ms(1000));
      fast_retx_inflight = false;
      dup_acks = 0;
    }
  }

  void on_ack(const Ack& a, MonoNs now) {
    if (!has_ack) {
      // First ACK ever: it is progress by definition (even value 0).
      has_ack = true;
      last_acked = a.ack_number;
      send_una = seq_next(a.ack_number);
      rto = kTcpRtoInit;
      head_retx = 0;
      fast_retx_inflight = false;
      dup_acks = 0;
      head_last_send = send_una < send_next ? now : -1;
      return;
    }
    if (!seq_gt(a.ack_number, last_acked)) {
      // Duplicate ACK: count toward fast retransmit of the head.
      if (a.ack_number == last_acked && send_una < send_next) {
        ++dup_acks;
        if (dup_acks >= kDupAckThreshold && !fast_retx_inflight) {
          retransmit_head(now);
          fast_retx_inflight = true;
          dup_acks = 0;
          rto = kTcpRtoInit;
        }
      }
      return;
    }
    // Cumulative progress.
    last_acked = a.ack_number;
    send_una = seq_next(a.ack_number);
    rto = kTcpRtoInit;
    head_retx = 0;
    fast_retx_inflight = false;
    dup_acks = 0;
    head_last_send = now;  // approximates restart of the head timer
    if (send_una >= send_next) head_last_send = -1;
  }

  // Receiver side of one arriving segment; returns frames completed (0 or 1).
  int consume(std::uint32_t seq, MonoNs now, std::uint32_t measured_from,
              std::uint32_t measured_to) {
    int completed = 0;
    const std::uint32_t prev_rcv_next = rcv_next;
    if (seq != rcv_next) {
      if (seq_gt(seq, rcv_next) && buffered.size() < kWindowSegs) {
        buffered.insert(seq);
      }
      // Duplicate ACK for the in-order position.
      send_ack_to_sender(now);
      return 0;
    }
    ++rcv_next;
    while (buffered.count(rcv_next) != 0) {
      buffered.erase(rcv_next);
      ++rcv_next;
    }
    send_ack_to_sender(now);
    // Frame completion: every frame boundary the drain crossed counts
    // (a large buffered drain can complete several frames at once).
    const std::uint32_t first_boundary = ((prev_rcv_next + 9) / 10) * 10;
    for (std::uint32_t boundary = first_boundary; boundary <= rcv_next;
         boundary += 10) {
      if (boundary == 0) continue;
      const std::uint32_t fid = boundary / 10 - 1;
      if (fid >= measured_from && fid < measured_to) {
        const MonoNs playout_time = playout(fid * kRtpIncrement);
        if (now <= playout_time + kNsPerMs) ++ontime; else ++late;
        ++delivered_frames;
        ++completed;
      }
    }
    return completed;
  }

  void send_ack_to_sender(MonoNs) {
    Header h;
    h.version = 1;
    h.header_len = 7;
    h.type = static_cast<std::uint8_t>(PacketType::ACK);
    h.stream_id = 1;
    h.payload_len = static_cast<std::uint16_t>(encoded_ack_size());
    Datagram ack;
    ack.peer = src_ep;
    ack.bytes.assign(kHeaderSize + encoded_ack_size(), std::byte{0});
    encode_header(ack.bytes.data(), h);
    Ack a;
    a.ack_number = rcv_next - 1;
    encode_ack(ack.bytes.data() + kHeaderSize, a);
    net_ptr->deliver_from(dst, ack);
  }
};

BenchResult run_tcp_arm(std::uint64_t seed, double loss, int burst) {
  BenchResult r;
  r.arm = "tcp-model";
  r.seed = seed;
  r.loss = loss;
  r.burst = burst;

  const rusage ru0 = now_rusage();
  VirtualClock clock;
  VirtualNetwork net(clock, seed);
  const auto gw_ep = Endpoint{htonl(0x0A000001), htons(50000)};
  const auto cl_ep = Endpoint{htonl(0x0A000002), htons(20000)};
  DirectionFaults down;
  down.data_loss = loss;
  down.burst_length = burst;
  down.base_delay = ns_from_ms(5);
  down.jitter = ns_from_ms(2);
  net.set_route(gw_ep, down);
  DirectionFaults up;
  up.base_delay = ns_from_ms(5);
  up.jitter = ns_from_ms(2);
  net.set_route(cl_ep, up);

  TcpModelArm tcp;
  tcp.clock_ptr = &clock;
  tcp.net_ptr = &net;
  tcp.src_ep = gw_ep;
  tcp.dst = cl_ep;

  constexpr std::uint32_t kSegsPerFrame = 10;
  const std::uint32_t total_segs =
      (kWarmupFrames + kMeasuredFrames) * kSegsPerFrame;
  std::uint32_t produced_segs = 0;
  std::uint64_t offered = 0;

  std::uint64_t tcp_dbg = 0;
  while (produced_segs < total_segs || tcp.rcv_next < total_segs) {
    if (++tcp_dbg % 200000 == 0) {
      std::cerr << "tcp t=" << clock.now() << " rcv_next=" << tcp.rcv_next
                << " una=" << tcp.send_una << " nxt=" << tcp.send_next
                << " buffered=" << tcp.buffered.size()
                << " retx=" << tcp.retransmissions
                << " rto_ms=" << tcp.rto / 1000000 << "\n";
    }
    MonoNs t = clock.now() + ns_from_ms(1);
    if (auto d = net.next_event_time()) t = std::min(t, *d);
    if (t <= clock.now()) t = clock.now() + 1;
    clock.set(t);

    for (auto& ev : net.pop_due(t)) {
      auto hdr = decode_header(ev.datagram.bytes);
      if (hdr.reject != RejectReason::NONE) continue;
      if (ev.dst == cl_ep) {
        if (tcp.anchor_arrival < 0) {
          tcp.anchor_arrival = t;
          tcp.anchor_rtp = 0;
        }
        tcp.consume(hdr.header.seq, t, kWarmupFrames,
                    kWarmupFrames + kMeasuredFrames);
      } else {
        auto a = decode_ack(std::span<const std::byte>(ev.datagram.bytes)
                                .subspan(kHeaderSize));
        if (a.reject == RejectReason::NONE) tcp.on_ack(a.ack, t);
      }
    }

    // Producer: frames due on the 30 fps schedule become segments.
    const std::uint32_t frames_due = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(t / (kNsPerSec / 30)) + 1,
        kWarmupFrames + kMeasuredFrames);
    const std::uint32_t target_segs = frames_due * kSegsPerFrame;
    while (produced_segs < target_segs) {
      ++produced_segs;
      const std::uint32_t fid = (produced_segs - 1) / kSegsPerFrame;
      if (fid >= kWarmupFrames && produced_segs % kSegsPerFrame == 0) {
        ++offered;  // count frames, not segments
      }
    }
    tcp.payload_done = produced_segs >= total_segs;
    tcp.send_window(t, produced_segs);
    tcp.on_timer(t);

  }
  // Final accounting: frames whose playout passed without delivery expired
  // (computed once; no incremental double counting).
  {
    const std::uint32_t playout_passed = tcp.anchor_arrival >= 0
        ? static_cast<std::uint32_t>(std::max<MonoNs>(
              0, (clock.now() - ns_from_ms(60) - tcp.anchor_arrival) /
                     static_cast<MonoNs>(kNsPerSec / 30)))
        : 0;
    const std::uint64_t expected =
        static_cast<std::uint64_t>(
            std::min<std::uint32_t>(playout_passed, kMeasuredFrames));
    const std::uint64_t counted = tcp.ontime + tcp.late;
    if (counted < expected) tcp.expired = expected - counted;
  }

  r.offered = offered;
  r.ontime = tcp.ontime;
  r.late = tcp.late;
  r.expired = tcp.expired;
  r.abandoned = tcp.retransmissions;
  r.net_dropped = net.stats().dropped;
  const std::uint64_t den = r.ontime + r.late + r.expired;
  r.fdr_playout =
      den ? static_cast<double>(r.ontime) / static_cast<double>(den) : 0.0;
  r.fdr_session = r.fdr_playout;
  const rusage ru1 = now_rusage();
  r.cpu_s = cpu_seconds(ru1) - cpu_seconds(ru0);
  r.mem_hwm_kb = static_cast<long>(ru1.ru_maxrss);
  return r;
}

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------
double mean(const std::vector<double>& v) {
  double s = 0;
  for (const double x : v) s += x;
  return v.empty() ? 0 : s / static_cast<double>(v.size());
}
double half_range(const std::vector<double>& v) {
  if (v.empty()) return 0;
  const auto [mn, mx] = std::minmax_element(v.begin(), v.end());
  return (*mx - *mn) / 2;
}

void append_json(std::ostringstream& os, const std::vector<BenchResult>& runs) {
  os << "  {\"arm\":\"" << runs[0].arm << "\",\"loss\":" << runs[0].loss
     << ",\"burst\":" << runs[0].burst << ",\"clients\":" << runs[0].clients
     << ",\"seeds\":[";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    os << runs[i].seed << (i + 1 < runs.size() ? "," : "");
  }
  os << "],\"runs\":[";
  for (std::size_t i = 0; i < runs.size(); ++i) {
    const auto& r = runs[i];
    os << "{\"seed\":" << r.seed << ",\"ontime\":" << r.ontime
       << ",\"late\":" << r.late << ",\"expired\":" << r.expired
       << ",\"abandoned_or_retx\":" << r.abandoned
       << ",\"offered\":" << r.offered << ",\"net_dropped\":" << r.net_dropped
       << ",\"fdr_session\":" << r.fdr_session
       << ",\"fdr_playout\":" << r.fdr_playout << ",\"p50_ms\":" << r.p50_ms
       << ",\"p95_ms\":" << r.p95_ms << ",\"p99_ms\":" << r.p99_ms
       << ",\"max_ms\":" << r.max_ms << ",\"cpu_s\":" << r.cpu_s
       << ",\"mem_hwm_kb\":" << r.mem_hwm_kb << "}"
       << (i + 1 < runs.size() ? "," : "");
  }
  os << "],";
  const auto fdr_of = [](const BenchResult& r) { return r.fdr_playout; };
  std::vector<double> fdrs, p99s;
  for (const auto& r : runs) {
    fdrs.push_back(fdr_of(r));
    p99s.push_back(r.p99_ms);
  }
  os << "\"fdr_playout_mean\":" << mean(fdrs)
     << ",\"fdr_playout_ci\":" << half_range(fdrs)
     << ",\"p99_ms_mean\":" << mean(p99s)
     << ",\"p99_ms_ci\":" << half_range(p99s) << "}";
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t base_seed = kBaseSeed;
  int repetitions = 5;
  double loss = 0.05;
  int burst = 0;
  int clients = 1;
  std::string out_prefix = "bench";
  bool fanout = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << arg << "\n";
        std::exit(2);
      }
      return argv[++i];
    };
    if (arg == "--seed") base_seed = std::stoull(next());
    else if (arg == "--reps") repetitions = std::stoi(next());
    else if (arg == "--frames") kMeasuredFrames = static_cast<std::uint32_t>(std::stoul(next()));
    else if (arg == "--loss") loss = std::stod(next());
    else if (arg == "--burst") burst = std::stoi(next());
    else if (arg == "--clients") clients = std::stoi(next());
    else if (arg == "--out") out_prefix = next();
    else if (arg == "--fanout") fanout = true;
    else if (arg == "--help") {
      std::cout << "usage: rudp_bench [--seed N] [--reps N] [--loss P]\n"
                   "                  [--burst N] [--clients N] [--out PREFIX]\n"
                   "                  [--fanout]\n";
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      return 2;
    }
  }

  std::vector<std::ostringstream> docs(1);
  std::ostringstream& json = docs[0];
  json << "{\n";

  if (!fanout) {
    // Arms 1+2: UDP / TCP-model / RUDP-RT / RUDP-RT-always-retransmit.
    std::vector<BenchResult> udp_runs, tcp_runs, rudp_runs, always_runs;
    for (int i = 0; i < repetitions; ++i) {
      const std::uint64_t seed = base_seed + static_cast<std::uint64_t>(i);
      udp_runs.push_back(run_udp_arm(seed, loss, burst));
      std::cout.flush(); std::cout << "udp seed=" << std::hex << seed << std::dec
                << " fdr=" << udp_runs.back().fdr_playout << "\n";
      tcp_runs.push_back(run_tcp_arm(seed, loss, burst));
      std::cout << "tcp-model seed=" << std::hex << seed << std::dec
                << " fdr=" << tcp_runs.back().fdr_playout << "\n";
      rudp_runs.push_back(run_rudp_arm("rudp", seed, loss, burst, 1, true));
      std::cout << "rudp seed=" << std::hex << seed << std::dec
                << " fdr=" << rudp_runs.back().fdr_playout << "\n";
      always_runs.push_back(run_rudp_arm("rudp-always", seed, loss, burst, 1,
                                         false));
      std::cout << "rudp-always seed=" << std::hex << seed << std::dec
                << " fdr=" << always_runs.back().fdr_playout << "\n";
    }

    json << "  \"workload\": {\"fps\":30,\"frame_bytes\":12000,"
         << "\"packets_per_frame\":10,\"rtt_ms\":10,\"jitter_ms\":2,"
         << "\"deadline_ms\":100,\"playout_ms\":60,\"warmup\":"
         << kWarmupFrames << ",\"measured\":" << kMeasuredFrames << "},\n";
    json << "  \"arms\":[\n";
    append_json(json, udp_runs);
    json << ",\n";
    append_json(json, tcp_runs);
    json << ",\n";
    append_json(json, rudp_runs);
    json << ",\n";
    append_json(json, always_runs);
    json << "\n  ]\n}\n";
  } else {
    // Arm 3: fan-out to three clients; per-session ledgers must conserve.
    std::vector<BenchResult> runs;
    for (int i = 0; i < repetitions; ++i) {
      const std::uint64_t seed = base_seed + static_cast<std::uint64_t>(i);
      runs.push_back(run_rudp_arm("rudp-fanout", seed, loss, burst, clients,
                                  true));
      std::cout << "fanout seed=" << std::hex << seed << std::dec
                << " fdr=" << runs.back().fdr_playout << "\n";
    }
    json << "  \"workload\": {\"clients\":" << clients << "},\n";
    json << "  \"arms\":[\n";
    append_json(json, runs);
    json << "\n  ]\n}\n";
  }

  const std::string out_path = out_prefix + ".json";
  std::ofstream out(out_path);
  out << json.str();
  std::cout << "wrote " << out_path << "\n";
  return 0;
}
