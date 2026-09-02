# ADR index

| ADR | Title | Status |
|---|---|---|
| 0001 | Capture FrameQueue full policy is producer-side drop-newest | Accepted |
| 0002 | Wire format v1 freeze (28-byte header + control payloads) | Accepted |
| 0003 | Session identity, epochs, handshake, pre-sync DATA handling | Accepted |
| 0004 | Deadline origin, wire encoding, admission early-exit | Accepted |
| 0005 | FDR accounting unit, ledgers, terminal classification | Accepted |
| 0006 | Receiver EXPIRED is delivery-only; frame-id gaps never block | Accepted |
| 0007 | Queue inventory, capacities, full policies | Accepted |
| 0008 | Per-session send scheduling, budgets, token buckets | Accepted |
| 0009 | Socket send/receive error mapping, pending datagrams | Accepted |
| 0010 | Timer model — single heap, entry tuple, tie-break, staleness | Accepted |
| 0011 | RTT sampling, RTO computation, per-entry backoff | Accepted |
| 0012 | Benchmark arms and FDR observation points | Accepted |
| 0013 | Control rate limiting and stale-control accounting | Accepted |
| 0014 | Configuration defaults, validation, TOML subset | Accepted |
| 0015 | Reference client behavior | Accepted |
| 0016 | Ownership and lifetime table | Accepted |
| 0017 | Admission early-exit threshold capped at D/2 | Accepted (Phase 2) |
| 0018 | RTT samples only on single-packet cumulative advances | Accepted (Phase 2) |
| 0019 | Frame dependency awareness, cascade abandon, broken GOP fast pruning | Accepted (Phase 6) |

