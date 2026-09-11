#!/usr/bin/env bash
# End-to-end smoke test for --video mode: real capture, real ONNX inference,
# real K-of-N confirmation, real gRPC delivery through the gateway. Requires
# a RIDGELINE_WITH_ONNX=ON build and the Phase 1b-ii assets fetched.
#
# The sample video (scripts/fetch-phase1b-assets.sh doesn't create this --
# generate it first) has exactly 4 dog segments, so a correct pipeline must
# deliver EXACTLY 4 DetectionEvents to the gateway, no more, no fewer.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50595}"
VIDEO="${2:-third_party/testdata/sample.avi}"
STATE_DIR="$(mktemp -d)/agent-state"
LOG_DIR="$(mktemp -d)"

if [[ ! -f "$VIDEO" ]]; then
  echo "SKIP: $VIDEO not found. Run: ./build/tools/ridgeline_make_sample_video"
  exit 0
fi

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" >"$LOG_DIR/gateway.log" 2>&1 &
GATEWAY_PID=$!
cleanup() { kill "$GATEWAY_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.5

"$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-video-smoke \
  --state-dir="$STATE_DIR" --video="$VIDEO" --classes=16 >"$LOG_DIR/agent.log" 2>&1
AGENT_STATUS=$?

sleep 0.3
kill -INT "$GATEWAY_PID"
GATEWAY_STATUS=0
wait "$GATEWAY_PID" || GATEWAY_STATUS=$?

echo "---- agent ----"; cat "$LOG_DIR/agent.log"
echo "---- gateway ----"; cat "$LOG_DIR/gateway.log"

[[ $AGENT_STATUS -ne 0 ]] && { echo "FAIL: agent exited $AGENT_STATUS"; exit 1; }
[[ $GATEWAY_STATUS -ne 0 ]] && { echo "FAIL: gateway exited $GATEWAY_STATUS"; exit 1; }
if ! grep -q "received=4 duplicates=0 lost=0" "$LOG_DIR/gateway.log"; then
  echo "FAIL: expected exactly 4 events, 0 duplicates, 0 lost (sample video has 4 dog segments)"
  exit 1
fi
echo "PASS: real video -> real inference -> real gRPC delivery, exactly 4/4 events, zero gaps"
