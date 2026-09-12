#!/usr/bin/env bash
# Regression test for ADR-0012 (config reconciliation). Verifies:
#   1. The gateway pushes a desired config to a device whose reported
#      applied_config_version is behind.
#   2. The agent actually applies it (rate change is large enough to be
#      unambiguous: 5Hz vs 15Hz over the same window can't be confused
#      with timing noise).
#   3. The gateway does NOT keep re-pushing the same config once the agent
#      reports it as applied -- a reconciliation loop that never converges
#      would eventually flood the stream with redundant config pushes.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50562}"
LOG_DIR="$(mktemp -d)"
CONFIG_FILE="$(mktemp)"

cat > "$CONFIG_FILE" <<'EOF'
# device_id,version,confirm_k,confirm_n,confidence_threshold,target_fps
cam-cfg-regress,7,3,5,0.35,15
EOF

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" --device-configs="$CONFIG_FILE" \
  >"$LOG_DIR/gateway.log" 2>&1 &
GW_PID=$!
cleanup() { kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.5

"$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-cfg-regress \
  --state-dir="$(mktemp -d)" --rate-hz=5 --duration-s=6 >"$LOG_DIR/agent.log" 2>&1
AGENT_STATUS=$?

sleep 0.3
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true

echo "---- agent ----"; cat "$LOG_DIR/agent.log"
echo "---- gateway ----"; grep -E "pushed config|cam-cfg-regress" "$LOG_DIR/gateway.log"

[[ $AGENT_STATUS -ne 0 ]] && { echo "FAIL: agent exited $AGENT_STATUS"; exit 1; }

if ! grep -q "applied config version=7: rate_hz=15.0" "$LOG_DIR/agent.log"; then
  echo "FAIL: agent should have applied the pushed config (rate_hz=15.0)"; exit 1
fi

TOTAL_SENT="$(grep -oP 'next_seq=\K[0-9]+' "$LOG_DIR/agent.log" | tail -1)"
# Started at 5Hz; config bumps to 15Hz partway through a 6s run. Even
# accounting for the ~1s delay before the first heartbeat carries the
# push, total events must be well above a flat 5Hz*6s=30 -- otherwise the
# rate change didn't actually take effect, it was just logged.
if [[ -z "$TOTAL_SENT" || "$TOTAL_SENT" -le 40 ]]; then
  echo "FAIL: total sent ($TOTAL_SENT) too low -- rate change doesn't appear to have taken effect"
  exit 1
fi

PUSH_COUNT="$(grep -c "pushed config version=7 to cam-cfg-regress" "$LOG_DIR/gateway.log")"
if [[ "$PUSH_COUNT" -ne 1 ]]; then
  echo "FAIL: expected exactly 1 config push (convergence), got $PUSH_COUNT"
  exit 1
fi

echo "PASS: config pushed once, applied (rate_hz=15.0), sent=$TOTAL_SENT (>40, rate change took effect), no repeat pushes"
