# ADR-0021: Fixed measurement windows and independent denominators

Status: Accepted, 2026-11-03. Supersedes ADR-0012's measurement details.

Every benchmark arm produces frame IDs `[0, 100 + N)`. Only `[100, 100 + N)`
contributes frame outcomes or latency samples; after production, allow a bounded
2-second recovery tail. RUDP additionally performs its shutdown closure/flush.
For `C` clients, playout FDR is unique ONTIME frame/client pairs divided by `N*C`.
Unseen or unfinished pairs are EXPIRED at the final observation barrier. This
includes complete frame loss, pre-admission loss, and missing sessions, rather
than silently shrinking the denominator to frames visible at the receiver.

Session FDR remains ONTIME cumulative-ACK closures divided by independently
observed session admissions (including pre-admission drops). `session_offered`
is reported separately: during a session outage it may differ from source
`offered`. ACK closure is neither decoded playback nor a receiver timestamp.

The receive pipeline keeps 512 terminal frame IDs (greater than the configured
256-packet receive horizon); subsequent fragments still advance transport state
but cannot recreate an already expired assembly or count a second terminal.
Optional synchronous observers expose events without retaining unbounded runtime
history. The benchmark retains its own finite, measured frame records.

Session ledger input is incremented at the fan-out entrance, independently of
sender admissions and closures. After routing each output batch, an imbalance
aborts. The source ledger's runtime total remains derived from consumer admissions
and capture drops; it is not an independent producer-versus-consumer audit.
The existing watchdog detects stalled ACK progress, not ledger imbalance.

Wire bytes and DATA/retransmission attempts cover the whole run, including warmup
and control traffic. CPU seconds measure simulator execution; RSS is process-wide
high water, shared across sequential arms. Neither is a gateway capacity claim.
Five-seed spread is labeled half-range, not a statistical confidence interval.
Hexadecimal and decimal seed syntax must consume the complete argument.
