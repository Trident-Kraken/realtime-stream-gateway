# Real socket weak-network experiments

Separate `rudp_gateway` and `rudp_client` processes, connected through a
userspace loopback UDP fault proxy. Gateway/client use their production
epoll/timerfd/eventfd paths and real monotonic clocks.

Source revision: `a4b80a8d884215568b729d272c347672bb82b585` plus `tools/socket_experiment.py` in this commit.
Host: `Linux-6.18.33.2-microsoft-standard-WSL2-x86_64-with-glibc2.39`; Python 3.12.3.

## Conditions

- Synthetic 30 fps, 12 KB/frame; default runtime configuration; 3 seconds
  of gateway operation per cell; clients stay 1.2 seconds longer for closure.
- Five scenarios × three seeds 24301..24303; fanout uses three clients.
- Proxy adds 5 ±2 ms delay in each direction. Loss/outage affects downstream
  DATA only; ACK/NACK/ABANDON/handshake traffic is delayed, not intentionally lost.
- Outage drops DATA at proxy time 800..1200 ms; its actual timing differs
  slightly from gateway capture time. No bandwidth bottleneck is modeled.
- Denominator is the independent gateway `session_offered` at shutdown.
  It includes admission drops; packets arriving before JOIN create no session offer.
  This is a time-bounded runtime experiment, not the simulation frame window.
- The numerator is receiver on-time playout count. Unobserved offers stay
  in the denominator. Session ACK FDR is reported separately in raw JSON.

## Results

| Scenario | Offers per run | Playout FDR mean [min, max] | Actual DATA loss | Retransmission share |
|---|---|---:|---:|---:|
| no-loss | 90, 90, 90 | 1.0000 [1.0000, 1.0000] | 0.00% | 12.53% |
| iid-5 | 90, 90, 90 | 0.9778 [0.9667, 0.9889] | 4.62% | 19.90% |
| burst-20-4 | 90, 90, 89 | 0.8140 [0.7753, 0.8444] | 24.28% | 29.28% |
| outage-400ms | 90, 90, 90 | 0.8852 [0.8778, 0.8889] | 19.36% | 20.72% |
| fanout-3 | 267, 267, 270 | 0.9963 [0.9888, 1.0000] | 0.00% | 13.23% |

All processes exit successfully with final metrics; all sessions have zero
in-flight frames at shutdown; watchdog/resource/queue-overflow counters are zero.
The proxy bounds its send heap to 4096 datagrams and peers to 16. Raw gateway
and client snapshots, process logs, proxy packet events and per-cell result.json
are retained in each scenario directory.

## Limits and reproduction

These are short functional weak-network trials with about 90 frames/client,
not a statistically powered performance study or a maximum-capacity benchmark.
Burst results may differ materially from simulation because timing, packet
order, batching, NACK scheduling and random draws differ. Real-clock replay is
not bit-identical, even for the same seed. No real TCP baseline or real codec
is included. No claim of production deployment follows from these measurements.

```bash
python3 tools/socket_experiment.py --reps 3 --duration 3000
python3 tools/summarize_socket.py
ctest --test-dir build -R '^system.socket_proxy' --output-on-failure
```

Requires local UDP sockets, but no root privileges, network-namespace changes
or traffic-control configuration. `--out` can point to a temporary directory.
