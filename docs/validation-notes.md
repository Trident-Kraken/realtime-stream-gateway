# Validation notes — 2026-11-23

## Measurement corrections (2026-11-03)

Schema v1 used inconsistent measurement windows, counted TCP frame boundaries
twice, and parsed hexadecimal seeds as zero. Those results are superseded by
[the schema-v2 report](experiments/results.md).

The corrected benchmark uses a fixed frame-ID interval and a source-frame/client
denominator. Missing frames stay in that denominator. Session entrance counts
are independent of terminal counters. Receiver terminal events are deduplicated
within the receive horizon. Regression cases cover no loss, complete loss,
burst loss, fanout and equivalent hexadecimal/decimal seeds.

## Reproduction and runtime checks

- Virtual-clock benchmarks use seeds 24301..24305. Fault cells use 24301 and
  exactly 1,000 source frames followed by shutdown closure. Both benchmark
  comparison and storm replay passed.
- Fifteen real-clock trials cover five scenarios and three seeds. Raw snapshots,
  process logs, a sample packet trace, and conditions are in
  [the socket report](experiments/socket/README.md).
- The controlled GOP trace closes frames 0/1/2 as abandoned, prunes frame 3,
  and plays frames 4/5 on time. Its JSON replay is identical.
- The current suite has 118 tests. The full local run passed in 27.76 seconds
  (`-DRUDP_WERROR=ON`, 0 warnings).

## Sanitizers

Sanitizer options previously appeared after target creation and did not instrument
those targets. The build now applies options before target creation, makes UBSan
fatal (`-fno-sanitize-recover=undefined`), and CI checks both compilation flags
and runtime linkage alongside full `system.*` coverage under ASan/UBSan and TSan.

ASan/UBSan passed all 118/118 tests locally in 89.26 seconds. This host's TSan
runtime fails at startup with `unexpected memory mapping`; TSan executes
successfully on the GitHub runner.
[CI run 45200470433](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/45200470433)
passed the normal build/test and ASan/UBSan/TSan jobs.

## Related changes

| Change | Commit | CI |
|---|---|---|
| Hardening & 118-test suite | de35f50 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/45200470433) |
| Verification | ccb40c5 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41608074949) |
| Experiments | a4b80a8 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41608625123) |
| Real sockets | e070834 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41609499948) |
| GOP demo | d51abe0 | [passed](https://github.com/Trident-Kraken/realtime-stream-gateway/actions/runs/41610192705) |
