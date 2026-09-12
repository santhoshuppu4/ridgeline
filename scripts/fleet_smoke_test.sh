#!/usr/bin/env bash
# Deterministic correctness check for the device simulator: a small fleet,
# no fault injection, every sent event must be acked. This is NOT a
# performance benchmark -- CI runners have few, often shared/throttled
# cores, so throughput/latency numbers from here are meaningless. Run
# ridgeline_device_simulator directly with a larger --devices count on real
# hardware for actual fleet-scale numbers (see context/adr/0009).
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50576}"
LOG_DIR="$(mktemp -d)"

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" >"$LOG_DIR/gateway.log" 2>&1 &
GW_PID=$!
cleanup() { kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.5

OUTPUT="$("$BUILD_DIR/tools/ridgeline_device_simulator" --gateway="127.0.0.1:$PORT" --devices=10 --duration-s=5 --rate-hz=2 --connect-stagger-ms=5)"
echo "$OUTPUT"

kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true

SENT="$(echo "$OUTPUT" | grep -oP 'sent:\s+\K[0-9]+')"
ACKED="$(echo "$OUTPUT" | grep -oP 'acked:\s+\K[0-9]+')"
[[ -z "$SENT" || -z "$ACKED" ]] && { echo "FAIL: could not parse sent/acked from simulator output"; exit 1; }
[[ "$SENT" -eq 0 ]] && { echo "FAIL: simulator sent zero events"; exit 1; }
[[ "$SENT" != "$ACKED" ]] && { echo "FAIL: sent=$SENT acked=$ACKED -- with no fault injection these must match exactly"; exit 1; }
echo "PASS: $ACKED/$SENT events acked across 10 simulated devices, no fault injection"
