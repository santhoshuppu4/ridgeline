#!/usr/bin/env bash
# Regression test for ADR-0010: the gateway must suppress a false "lost"
# gap when a device honestly declares it has no durable resume state
# (durable_resume=false), while STILL catching a genuine gap when a device
# claims real resume state (durable_resume=true) and the numbers don't add
# up. Testing only one direction would be worse than testing neither --
# a fix that just stops checking gaps entirely would "pass" a
# durable_resume=false-only test while silently disabling real gap
# detection.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50573}"
LOG_DIR="$(mktemp -d)"

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" >"$LOG_DIR/gateway.log" 2>&1 &
GW_PID=$!
cleanup() { kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.5

"$BUILD_DIR/tools/ridgeline_gap_protocol_test" --gateway="127.0.0.1:$PORT" --device-id=dev-honest \
  --durable-resume=false --last-acked-seq=0 --send-seq=25
"$BUILD_DIR/tools/ridgeline_gap_protocol_test" --gateway="127.0.0.1:$PORT" --device-id=dev-claims-resume \
  --durable-resume=true --last-acked-seq=0 --send-seq=25

sleep 0.3
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true

echo "---- gateway log ----"; grep -E "dev-honest|dev-claims-resume" "$LOG_DIR/gateway.log"

if ! grep -q "dev-honest disconnected: received=1 duplicates=0 lost=0" "$LOG_DIR/gateway.log"; then
  echo "FAIL: durable_resume=false device should show lost=0 (no false gap)"; exit 1
fi
if ! grep -q "dev-claims-resume disconnected: received=1 duplicates=0 lost=24" "$LOG_DIR/gateway.log"; then
  echo "FAIL: durable_resume=true device with a real gap should show lost=24"; exit 1
fi
echo "PASS: false gap suppressed for durable_resume=false; real gap still caught for durable_resume=true"
