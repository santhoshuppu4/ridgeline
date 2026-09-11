#!/usr/bin/env bash
set -euo pipefail
BUILD_DIR="${1:-build}"
PORT="${PORT:-50599}"
LOG_DIR="$(mktemp -d)"
"$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-smoke --rate-hz=50 --duration-s=8 \
  >"$LOG_DIR/agent.log" 2>&1 &
AGENT_PID=$!
sleep 3  # longer than the agent's 2s connect timeout, forcing at least one backoff
"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" >"$LOG_DIR/gateway.log" 2>&1 &
GATEWAY_PID=$!
cleanup() { kill "$AGENT_PID" "$GATEWAY_PID" 2>/dev/null || true; }
trap cleanup EXIT
status=0; wait "$AGENT_PID" || status=$?
kill -INT "$GATEWAY_PID"; gateway_status=0; wait "$GATEWAY_PID" || gateway_status=$?
echo "---- agent ----"; cat "$LOG_DIR/agent.log"
echo "---- gateway ----"; cat "$LOG_DIR/gateway.log"
[[ $status -ne 0 ]] && { echo "FAIL: agent exited $status"; exit 1; }
[[ $gateway_status -ne 0 ]] && { echo "FAIL: gateway exited $gateway_status"; exit 1; }
grep -q "retrying in" "$LOG_DIR/agent.log" || { echo "FAIL: backoff path not exercised"; exit 1; }
echo "PASS"
