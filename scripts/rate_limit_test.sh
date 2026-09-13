#!/usr/bin/env bash
# Regression test for the rate-limiting half of ADR-0013. A device sends at
# 20Hz against a gateway configured for capacity=5, refill=2/sec -- a
# sustained rate far above what the bucket allows. Verifies:
#   1. Some events are genuinely rejected (rate_limited > 0) -- the limiter
#      is actually enforced, not just configured and ignored.
#   2. NOT every event is rejected (received > 0) -- it's throttling, not a
#      total block.
#   3. "lost" (unexplained gap) is exactly 0 -- every skipped sequence
#      number is attributed to rate limiting, not misreported as data loss.
#      This is the ADR-0013 fix: an earlier version reported lost=42 for
#      this exact scenario despite zero real loss, repeating (within a
#      single stream) the same mistake ADR-0010 fixed across reconnects.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50558}"
LOG_DIR="$(mktemp -d)"

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" \
  --rate-limit-capacity=5 --rate-limit-per-second=2 >"$LOG_DIR/gateway.log" 2>&1 &
GW_PID=$!
cleanup() { kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT
sleep 0.5

if ! grep -q "rate limiting enabled: capacity=5.0 tokens_per_second=2.0" "$LOG_DIR/gateway.log"; then
  echo "FAIL: gateway did not report rate limiting enabled"; cat "$LOG_DIR/gateway.log"; exit 1
fi

timeout 8 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-ratelimit \
  --state-dir="$(mktemp -d)" --rate-hz=20 --duration-s=3 >"$LOG_DIR/agent.log" 2>&1 || true
# The agent's own --duration-s is a documented SOFT limit (ADR-0011): most
# events here are deliberately rate-limited and never acked, so the
# agent's exit condition (outbox must be empty) never becomes true on its
# own. Wrapped in an external `timeout`, same as every other script in
# this project that exercises a scenario where delivery can't complete --
# this is the known, accepted behavior, not something this test is meant
# to validate.

sleep 0.3
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true

echo "---- gateway ----"; grep "cam-ratelimit disconnected" "$LOG_DIR/gateway.log"

LINE="$(grep "cam-ratelimit disconnected" "$LOG_DIR/gateway.log")"
RECEIVED="$(echo "$LINE" | grep -oP 'received=\K[0-9]+')"
LOST="$(echo "$LINE" | grep -oP 'lost=\K[0-9]+')"
RATE_LIMITED="$(echo "$LINE" | grep -oP 'rate_limited=\K[0-9]+')"

[[ "$RECEIVED" -eq 0 ]] && { echo "FAIL: received=0 -- rate limiter blocked everything, not just throttled"; exit 1; }
[[ "$RATE_LIMITED" -eq 0 ]] && { echo "FAIL: rate_limited=0 -- limiter had no effect at 20Hz against capacity=5/2-per-sec"; exit 1; }
[[ "$LOST" -ne 0 ]] && { echo "FAIL: lost=$LOST -- rate-limited events must be attributed, not reported as data loss"; exit 1; }

echo "PASS: received=$RECEIVED, rate_limited=$RATE_LIMITED, lost=0 (all skips correctly attributed to rate limiting)"
