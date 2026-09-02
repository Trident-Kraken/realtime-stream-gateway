# Experiment results and conclusions (v1, seed 0x5EED)

All numbers below come from the deterministic virtual-clock runner with the
fixed workload of spec §8.2: 30 fps, 12,000 media bytes/frame, 10 DATA
packets/frame, 10 ms base RTT ±2 ms jitter, 100 ms sender deadline, 60 ms
playout delay, 100-frame warm-up, then the measured window. P99 latency is
the delivered-frame latency over the RTP-anchored receiver timeline.
Raw per-cell JSON and traces are in this directory.

**Observation points.** Per spec §6 the authoritative FDR is
`session_ontime / session_offered` from the gateway's session ledger — the
only point where the full denominator (including pre-admission drops) is
observable and the two conservation equations hold continuously. The
reference client additionally reports a playout-side FDR
(ontime / (ontime+late+expired+abandoned) over client-visible frames); it is
reported alongside, not mixed, with the authoritative figure.

## Fault matrix (1000-frame cells; every row: conservation ✓, watchdog 0)

| Scenario | Seed | Offered | Ontime | Late | Aband. | Expired | Pre-adm. | FDR(session) | FDR(playout) | P99 ms |
|---|---|---|---|---|---|---|---|---|---|---|
| asymmetric | 0x0 | 1393 | 1384 | 0 | 1 | 0 | 7 | 0.994 | 0.999 | 60 |
| burst-2 | 0x0 | 1393 | 1243 | 0 | 142 | 0 | 7 | 0.892 | 0.837 | 60 |
| burst-4 | 0x0 | 1393 | 1284 | 0 | 101 | 0 | 7 | 0.922 | 0.901 | 60 |
| burst-8 | 0x0 | 1394 | 1316 | 0 | 68 | 0 | 7 | 0.944 | 0.947 | 60 |
| control-loss | 0x0 | 1393 | 1383 | 0 | 2 | 0 | 7 | 0.993 | 0.998 | 60 |
| delay | 0x0 | 1394 | 1389 | 0 | 2 | 0 | 1 | 0.996 | 1.000 | 60 |
| duplicate | 0x0 | 1393 | 1384 | 0 | 1 | 0 | 7 | 0.994 | 0.999 | 60 |
| iid-0 | 0x0 | 1393 | 1385 | 0 | 0 | 0 | 7 | 0.994 | 1.000 | 60 |
| iid-001 | 0x0 | 1393 | 1384 | 0 | 1 | 0 | 7 | 0.994 | 0.999 | 60 |
| iid-005 | 0x0 | 1393 | 1385 | 0 | 0 | 0 | 7 | 0.994 | 1.000 | 60 |
| iid-010 | 0x0 | 1393 | 1370 | 0 | 15 | 0 | 7 | 0.983 | 0.983 | 60 |
| iid-020 | 0x0 | 1393 | 1268 | 0 | 116 | 0 | 7 | 0.910 | 0.866 | 60 |
| jitter | 0x0 | 1393 | 1385 | 0 | 0 | 0 | 7 | 0.994 | 1.000 | 60 |
| reorder | 0x0 | 1393 | 1382 | 0 | 3 | 0 | 7 | 0.992 | 0.996 | 60 |
| storm | 0x0 | 1393 | 1362 | 0 | 23 | 0 | 7 | 0.978 | 0.767 | 60 |

Conclusions:

1. **Deadline-aware ABANDON keeps the window live under loss.** Abandonment
   grows with loss (0 at 0% → 116 frames at 20%) and every abandoned frame's
   sequences reach the terminal consumed state (INV-01 holds in all rows; the
   send window drains — no wedge).
2. **P99 delivered-frame latency is pinned at the 60 ms playout delay** in
   every row: on-time delivery never waits on recovery; frames that would
   miss playout are abandoned instead (well inside the 150 ms release bound).
3. Recovery is NACK-driven (≈1 RTT); RTO is the liveness backstop.

## Release targets (fixed workload, 5 seeds 0x5EED..+4, mean ± half-range)

| Target (spec §8.2) | Observation point | Result | Verdict |
|---|---|---|---|
| IID 5% loss: FDR ≥ 95% | session ledger | 0.999 ± 0.000 | **PASS** |
| IID 5% loss: P99 delivered latency ≤ 150 ms | receiver RTP-anchored | 60 ms | **PASS** |
| Burst 20% loss, length 4: FDR ≥ 85% | session ledger | 0.960 ± 0.007 | **PASS** |
| Deadline arm strictly more on-time frames than always-retransmit (same RTO) | both points | session 0.960 > 0.956; playout 0.682 > 0.672 | **PASS** |
| Zero watchdog violations / continuously valid conservation | all runs | 0 violations; conservation held | **PASS** |

Supplementary (non-authoritative) client-playout FDR at burst 20/4 is
0.682 ± 0.006: the gap to the session figure is frames the sender delivered
within the 100 ms deadline but whose recovery finished after the receiver's
60 ms playout instant (the deadline and playout clocks are deliberately
independent — v0.2 D-23). Under burst loss the RTO backoff (min 50 ms, per
entry) cannot always beat 60 ms; the deadline arm's ABANDON is what keeps the
window draining so later frames still deliver.

## Arm comparison at 5% IID loss (fixed workload, mean over 5 seeds)

| Arm | On-time FDR (playout view) | Notes |
|---|---|---|
| UDP | 0.601 ± 0.005 | ≈ 0.95^10 per-frame delivery probability — no recovery |
| TCP-model (ADR-0012) | 0.651 ± 0.037 | ordered byte stream, buffered reordering, dup-ACK fast retransmit, RTO_MIN 200 ms: head-of-line blocking turns every unrecovered hole into a full-window stall |
| RUDP-RT (deadline) | 0.973 ± 0.002 | out-of-order delivery + NACK fast recovery + ABANDON |
| RUDP-RT (always) | 0.973 ± 0.001 | same transport without the deadline abandon |

The TCP-model arm is a model, not kernel TCP (ADR-0012): it reproduces the
head-of-line-blocking property that matters for this workload comparison and
is labeled as a model everywhere. Its 0.651 at 5% loss reflects RTO-scale
repair (min 200 ms) after ACK-path losses; the deadline-aware RUDP-RT arm
recovers within the playout window and abandons what cannot make it, which
also yields strictly more on-time frames than its own always-retransmit arm.

## Fan-out (three clients, 5% loss)

Per-session ledgers conserve independently and simultaneously
(`bench-fanout.json`): every session-frame of every client reaches exactly
one terminal outcome; the client-playout FDR matches the single-client run
(0.973 ± 0.000).

## Reproducibility

Every cell records seed and parameters. `tools/run_fault_matrix.sh` re-runs
one cell twice and diffs the JSON — the check passes (bit-identical), which
is the Phase 5 reproducibility gate. The benchmark runner performs the same check over its simulated metrics (host CPU/RSS measurement fields are excluded — they measure the test machine, not the protocol).
