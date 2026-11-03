#!/usr/bin/env bash
# Benchmark runner (spec §8.2 fixed workload; release targets).
#   - UDP / TCP-model / RUDP-RT comparison at 5% IID loss
#   - deadline-aware vs always-retransmit (same RTO configuration)
#   - three-client fan-out
#   - burst-20%-len-4 row (release target 2)
set -euo pipefail
OUT=${1:-docs/experiments}
BIN=${BIN:-build/rudp_bench}
mkdir -p "$OUT"
SEED=${SEED:-0x5EED}

"$BIN" --seed "$SEED" --reps 5 --loss 0.05 --out "$OUT/bench-comparison"
"$BIN" --seed "$SEED" --reps 5 --loss 0.20 --burst 4 --out "$OUT/bench-burst"
"$BIN" --seed "$SEED" --reps 3 --loss 0.05 --clients 3 --fanout \
  --out "$OUT/bench-fanout"

# Determinism check: identical seeds reproduce identical *simulated* metrics.
# Host-dependent measurement fields (cpu_s, mem_hwm_kb) are excluded.
"$BIN" --seed "$SEED" --reps 1 --loss 0.05 --out "$OUT/bench-det-a" > /dev/null
"$BIN" --seed "$SEED" --reps 1 --loss 0.05 --out "$OUT/bench-det-b" > /dev/null
python3 "$(dirname "$0")/compare_benchmarks.py" "$OUT/bench-det-a.json" "$OUT/bench-det-b.json"
rm -f "$OUT/bench-det-a.json" "$OUT/bench-det-b.json"
echo "benchmarks complete: $OUT"
