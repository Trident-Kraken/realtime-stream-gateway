# RUDP-RT — Deterministic, Deadline-Aware Reliable Media Transport


[English](README.md) | [简体中文](README_zh.md)

[![CI](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/workflows/ci.yml/badge.svg)](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/workflows/ci.yml)


![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)
![Platform](https://img.shields.io/badge/platform-Linux-orange.svg)
![License](https://img.shields.io/badge/license-MIT-green.svg)

A high-performance Linux media gateway and transport protocol implementation of **RUDP-RT**: a reliable transport tailored for real-time media that trades unconditional delivery guarantees for *bounded-latency, useful on-time delivery*.

It combines Linux `epoll` single-writer event loops with custom RUDP reliability: ACK/SACK/NACK selective recovery, sliding-window flow control, RFC 6298 dynamic RTT/RTO tracking, per-frame deadlines, an explicit **ABANDON** protocol handshake to eliminate send-window head-of-line blocking (Wedge), and **GOP-level frame dependency awareness** with cascade abandon and broken-GOP pre-admission pruning (ADR-0019).

```
┌────────────────────────────────────────────────────────────────────────┐
│                   Media Source Layer (Synthetic / V4L2)                │
│         - GOP Structuring (KeyFrame / DeltaFrame)  - PLI Signaling     │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │ SPSC Ring (Drop-newest Overload Policy)
┌──────────────────────────────────▼─────────────────────────────────────┐
│                 Linux Runtime Engine (Reactor / EventLoop)             │
│    - Single-Writer Protocol Engine   - epoll + timerfd + signalfd Loop │
│    - Non-blocking UDP (EAGAIN Retry) - TokenBucket Rate Limiter        │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                  Transport Core (RUDP-RT State Machine)                │
│ - 28-Byte Fixed Binary Header      - RFC 1982 Serial Number Arithmetic │
│ - ACK / SACK (Bitmap) / NACK Fast  - RFC 6298 RTO (Single-packet sample│
│ - Frame Deadline Lifetime Manager  - Explicit ABANDON Protocol Handshake│
│ - Transitive Cascade Abandon       - Broken-GOP Pre-Admission Pruning  │
└──────────────────────────────────┬─────────────────────────────────────┘
                                   │
┌──────────────────────────────────▼─────────────────────────────────────┐
│                 Receiver Pipeline (PlayoutPipeline)                    │
│ - Packet Classification / Filter   - Frame Assembler (Lossless reorder)│
│ - Playout Timeline JitterBuffer    - Dual-Ledger Conservation Invariants│
└────────────────────────────────────────────────────────────────────────┘
```

---

## Key Highlights

- **Single-Writer Concurrency:** Lockless protocol state transitions. The network reactor runs on a dedicated thread, driving session states, timers, and packet processing without mutex contention or data races.
- **Deadline-Aware & Explicit ABANDON:** Frames that exceed their transmission deadline are cleanly abandoned. Obsolete sequence intervals are explicitly signaled via `ABANDON` datagrams, allowing the receiver to advance its window without wedging sender or receiver buffers.
- **GOP Dependency Awareness & Cascade Abandon:** Distinguishes between independently decodable KeyFrames and dependent DeltaFrames. When a KeyFrame times out, its entire downstream GOP dependency chain is cascade-abandoned, preventing useless traffic from congesting the link.
- **RFC 6298 RTT Estimation without HOLB Pollution:** Samples RTT exclusively on single-packet cumulative advances (ADR-0018), avoiding SRTT inflation caused by packet loss recovery delays.
- **Dual-Ledger Accounting Conservation (INV-20/INV-21):**
  $$\text{Offered} = \text{Ontime} + \text{Late} + \text{Abandoned} + \text{Expired} + \text{Pre-admission} + \text{In-flight}$$
  Every frame transitions through strictly checked conservation invariants verified by continuous runtime watchdogs.
- **100% Deterministic Simulation:** Virtual clock and virtual network abstractions enable bit-identical metric replays across runs under arbitrary synthetic loss, burst drop, jitter, and reordering.

---

## What is in the Box

| Binary | Purpose |
|---|---|
| `rudp_gateway` | Multi-client media gateway (non-blocking UDP, `epoll`, single `timerfd` min-heap, SPSC capture queue, single-writer protocol state) |
| `rudp_client` | Reference receiver: ACK/SACK/NACK generator, frame assembler, static jitter buffer, playout metrics |
| `rudp_sim` | Deterministic in-process virtual-clock/network simulation runner used by integration and fault tests |
| `rudp_bench` | Reproducible benchmark suite (UDP vs TCP-model vs RUDP-RT under varying loss and deadline profiles) |

---

## Quick Start

### Build

```bash
# Build release binaries
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j8

# Run test suite (83 tests: unit, property, integration, fault, system)
ctest --test-dir build -j1
```

### Sanitizer Verification

```bash
# AddressSanitizer & UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=address -DRUDP_WERROR=OFF
cmake --build build-asan -j8 && ctest --test-dir build-asan -j1 -E "system\."

# ThreadSanitizer
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DRUDP_SANITIZE=thread -DRUDP_WERROR=OFF
cmake --build build-tsan -j8 && ctest --test-dir build-tsan -j1 -R "unit|integration|fault"
```

### Running Live Gateway & Client

In terminal 1 (Gateway):
```bash
./build/rudp_gateway --port 50000 --duration 10000 --metrics-out /tmp/gw.jsonl
```

In terminal 2 (Client):
```bash
./build/rudp_client --port 50000 --duration 8000 --metrics-out /tmp/cl.jsonl
```

Both processes output a final immutable JSON metrics snapshot detailing packet counters, session ledgers, on-time FDR, and zero-violation watchdog reports.

---

## Reproducible experiments (schema v2)

30 fps, 12 KB/frame, 10 ms base RTT; 100 warmup + 10,000 measured frames,
five seeds `24301..24305`. Fixed-denominator receiver playout FDR means below
include wholly lost frames and pre-admission drops.

| Scenario | UDP | TCP-model | RUDP deadline | RUDP always |
|---|---:|---:|---:|---:|
| IID 5% | 0.5959 | 0.5139 | 0.9868 | 0.9872 |
| Burst 20% / 4 | 0.4414 | 0.0000 | 0.8630 | 0.8628 |

TCP-model is a userspace ordered-byte-stream model, not Linux TCP performance.
Deadline-mode gains vary by seed; these runs do not establish a robust general
advantage over always-retransmit. The 60 ms P99 uses a first-arrival-anchored
playout timeline, not synchronized capture or decoded-video latency.
See [full results](docs/experiments/results.md) for per-seed data, half-ranges,
retransmission cost, limits and reproduction commands.

---

## Documentation Map

- [`docs/SPECIFICATION.md`](docs/SPECIFICATION.md) — Normative architecture and implementation specification
- [`docs/protocol.md`](docs/protocol.md) — Wire format v1, packet structures, and state transitions
- [`docs/adr/`](docs/adr/) — 20 Architecture Decision Records (ADR-0001 to ADR-0020)
- [`docs/DEV-LOG.md`](docs/DEV-LOG.md) — Engineering milestones, postmortems, and debugging records
- [`docs/experiments/`](docs/experiments/) — Fault matrix, benchmark traces, and comparative analysis
- [`CONTEXT.md`](CONTEXT.md) — Domain glossary and unified terminology

---

## License

This project is licensed under the [MIT License](LICENSE).
