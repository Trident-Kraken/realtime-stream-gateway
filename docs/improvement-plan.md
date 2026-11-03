# Recruitment project strengthening

Each step is validated, documented and pushed separately to `origin/main`.

1. **Verification foundations (complete):** effective sanitizer instrumentation;
   fixed frame-id measurement window and independent denominator; unique playout
   terminal events; independent session entrance count; seed parsing; fail-closed
   deterministic comparison; regression tests.
2. **Reproduce experiments:** regenerate raw JSON/traces, publish conditions,
   multiple seeds, playout/session distinctions and wire/retransmission overhead.
3. **Real socket weak-network evidence:** bounded userspace UDP fault proxy,
   separate gateway/client processes, measured results and limitations.
4. **Visible demonstration:** replayable frame timeline showing loss, abandonment
   and recovery, using actual recorded events.
5. **Presentation:** align bilingual READMEs with evidence and add a resume/interview
   guide with honest scope and remaining limitations.

Historical schema-v1 benchmark numbers are withdrawn pending step 2. They used
inconsistent windows, a duplicated TCP frame boundary, and decimal-only parsing
of hexadecimal seeds. Their labels must not be used as current performance claims.

Step 1 validation (Linux/GCC 13.3): 83 normal tests pass; all 78 socket-free
ASan/UBSan tests pass. Instrumentation/linkage checks pass for ASan/UBSan and
TSan. The local TSan runtime cannot start (`unexpected memory mapping`), including
outside the sandbox; this is recorded as an environment limitation, not a pass.
GitHub CI runs instrumentation checks and sanitizer tests on its own runner.
