#!/usr/bin/env bash
# Benchmark runner (spec §8.2 fixed workload; release targets).
#   - UDP / TCP-model / RUDP-RT comparison at 5% IID loss
#   - deadline-aware vs always-retransmit (same RTO configuration)
#   - three-client fan-out
#   - burst-20%-len-4 row (release target 2)
set -eu
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
strip_host_fields() {
  sed -E 's/"cpu_s":[0-9.]+//"cpu_s":0/g; s/"mem_hwm_kb":[0-9]+/"mem_hwm_kb":0/g' "$1"
}
"$BIN" --seed "$SEED" --reps 1 --loss 0.05 --out "$OUT/bench-det-a" > /dev/null
"$BIN" --seed "$SEED" --reps 1 --loss 0.05 --out "$OUT/bench-det-b" > /dev/null
if diff -q <(strip_host_fields "$OUT/bench-det-a.json") \
           <(strip_host_fields "$OUT/bench-det-b.json") > /dev/null; then
  echo "bench determinism check: OK"
else
  echo "bench determinism check: MISMATCH"; exit 1
fi
rm -f "$OUT/bench-det-a.json" "$OUT/bench-det-b.json"
echo "benchmarks complete: $OUT"
