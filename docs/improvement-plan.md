# Recruitment project strengthening

Each step is validated, documented and pushed separately to `origin/main`.

1. **Verification foundations (complete):** effective sanitizer instrumentation;
   fixed frame-id measurement window and independent denominator; unique playout
   terminal events; independent session entrance count; seed parsing; fail-closed
   deterministic comparison; regression tests.
2. **Reproduce experiments (complete):** regenerate raw JSON/traces, publish conditions,
   multiple seeds, playout/session distinctions and wire/retransmission overhead.
3. **Real socket weak-network evidence (complete):** bounded userspace UDP fault proxy,
   separate gateway/client processes, measured results and limitations.
4. **Visible demonstration (complete):** replayable frame timeline showing loss, abandonment
   and recovery, using actual recorded events.
5. **Presentation (complete):** align bilingual READMEs with evidence and add a resume/interview
   guide with honest scope and remaining limitations.

Historical schema-v1 benchmark numbers are withdrawn pending step 2. They used
inconsistent windows, a duplicated TCP frame boundary, and decimal-only parsing
of hexadecimal seeds. Their labels must not be used as current performance claims.

Step 1 validation (Linux/GCC 13.3): 83 normal tests pass; all 78 socket-free
ASan/UBSan tests pass. Instrumentation/linkage checks pass for ASan/UBSan and
TSan. The local TSan runtime cannot start (`unexpected memory mapping`), including
outside the sandbox; this is recorded as an environment limitation, not a pass.
GitHub CI runs instrumentation checks and sanitizer tests on its own runner.

Step 2: regenerated all benchmark and fault artifacts with seeds 24301..24305
(faults use 24301), fixed 1000-frame fault production and shutdown closure, and
confirmed both benchmark determinism and storm replay. The results report is
generated from raw JSON, including half-ranges and recovery traffic. Deadline
ablation is mixed across seeds; the report makes no universal improvement claim.

Remote validation of step 1: [GitHub Actions run 41608074949](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41608074949)
passed both the normal build/test job and the ASan/UBSan/TSan job. TSan executes
successfully on the GitHub runner; the mapping limitation is local to this host.

Step 3: 15 real-clock trials (five scenarios, three seeds) completed with separate
processes and a bounded loopback UDP proxy. Both new socket regression tests pass.
Raw snapshots/packet events and conditions are in `docs/experiments/socket/`.

Step 4: `rudp_demo` records a scripted GOP interruption and verifies abandoned
0/1/2, pruned 3, on-time 4/5 and a closed independent session ledger. The SVG is
rendered from JSON callbacks; JSON replay is identical. The demo exposed and fixed
an uncleared scalar keyframe-request flag in moved SenderOutput batches.

Step 5: rewrote both READMEs around verified scope, current results, commands and
limitations; added a Chinese recruitment narrative/resume/interview guide; aligned
glossary and historical-spec precedence. Current suite: 86 tests. Each prior step
was pushed to origin/main separately: ccb40c5, a4b80a8, e070834, d51abe0.

Final local validation: all 86 tests passed in 27.21 seconds; documentation links,
`git diff --check`, demo replay and storm replay passed. Remote CI for steps 1–4
completed successfully, including the sanitizer job:

| Step | Commit | CI |
|---|---|---|
| Verification | ccb40c5 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41608074949) |
| Experiments | a4b80a8 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41608625123) |
| Real sockets | e070834 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41609499948) |
| GOP demo | d51abe0 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41610192705) |
