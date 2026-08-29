#!/usr/bin/env bash
# Replay a recorded scenario trace and verify bit-identical results
# (deterministic virtual clock; spec Phase 5).
#   Usage: tools/replay_trace.sh docs/experiments/fault-iid-005.json
set -eu
TRACE=$1
OUT=${2:-/tmp/replay-check.json}
BIN=${BIN:-build/rudp_sim}

args=$(python3 - "$TRACE" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
p = d["params"]
args = []
if p.get("loss"): args += ["--loss", str(p["loss"])]
if p.get("burst"): args += ["--burst", str(p["burst"])]
if p.get("down_loss") is not None: args += ["--down-loss", str(p["down_loss"])]
if p.get("down_burst") is not None: args += ["--down-burst", str(p["down_burst"])]
if p.get("control_loss"): args += ["--control-loss", str(p["control_loss"])]
if p.get("jitter_ms"): args += ["--jitter", str(p["jitter_ms"])]
if p.get("reorder"): args += ["--reorder", str(p["reorder"])]
if p.get("duplicate"): args += ["--duplicate", str(p["duplicate"])]
if p.get("clients"): args += ["--clients", str(p["clients"])]
args += ["--seed", str(d["seed"]), "--frames", str(p.get("frames", 300))]
print(" ".join(args))
PY
)
BASE=$(dirname "$TRACE")
NAME=$(basename "$TRACE" .json); NAME=${NAME#fault-}
"$BIN" --scenario "$NAME" $args --json "$OUT" > /dev/null
if diff -q "$TRACE" "$OUT" > /dev/null; then
  echo "replay OK: $TRACE reproduced bit-identically"
else
  echo "replay MISMATCH for $TRACE"; diff "$TRACE" "$OUT" | head; exit 1
fi
