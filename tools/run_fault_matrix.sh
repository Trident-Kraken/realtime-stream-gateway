#!/usr/bin/env bash
# Fault-injection matrix runner (spec §8.2). Each cell runs the seeded
# virtual-clock scenario via rudp_sim; conservation and watchdog assertions
# are built into the binary (exit code 1 = invariant violation).
#
# Usage: tools/run_fault_matrix.sh [OUT_DIR]
set -u
OUT=${1:-docs/experiments}
BIN=${BIN:-build/rudp_sim}
mkdir -p "$OUT"

run() { # name seed extra-args...
  local name=$1 seed=$2; shift 2
  echo "=== $name (seed $seed)"
  if "$BIN" --scenario "$name" --seed "$seed" --json "$OUT/fault-$name.json" \
       --trace "$OUT/fault-$name.trace" "$@"; then
    echo "    OK"
  else
    echo "    INVARIANT FAILURE"; exit 1
  fi
}

SEED=${SEED:-0x5EED}
for loss in 0 0.01 0.05 0.10 0.20; do
  tag=$(echo "$loss" | tr -d '.')
  run "iid-$tag" "$SEED" --frames 1000 --loss "$loss"
done
for blen in 2 4 8; do
  run "burst-$blen" "$SEED" --frames 1000 --down-loss 0.20 --down-burst "$blen"
done
run "delay"    "$SEED" --frames 1000 --delay 25 --jitter 0
run "jitter"   "$SEED" --frames 1000 --delay 5  --jitter 3
run "reorder"  "$SEED" --frames 1000 --reorder 0.10 --down-loss 0.02
run "duplicate" "$SEED" --frames 1000 --duplicate 0.05 --down-loss 0.02
run "asymmetric" "$SEED" --frames 1000 --loss 0.10 --down-loss 0.01
run "control-loss" "$SEED" --frames 1000 --loss 0.02 --control-loss 0.10
run "storm"    "$SEED" --frames 1000 --down-loss 0.10 --down-burst 4 \
    --jitter 2 --reorder 0.05 --duplicate 0.03 --control-loss 0.03

# Determinism check: the same scenario+seed must produce identical output.
"$BIN" --scenario iid-005 --seed "$SEED" --frames 1000 --loss 0.05 \
  --json "$OUT/fault-iid-005-replay.json" > /dev/null
if diff -q "$OUT/fault-iid-005.json" "$OUT/fault-iid-005-replay.json" > /dev/null; then
  echo "=== determinism check: OK"
else
  echo "=== determinism check: MISMATCH"; exit 1
fi
echo "fault matrix complete: $OUT"
