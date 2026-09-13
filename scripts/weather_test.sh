#!/usr/bin/env bash
# Real end-to-end test against the LIVE Open-Meteo API -- this needs
# genuine internet access, which this project's own development sandbox
# does not have (a direct curl to api.open-meteo.com from there returns
# "Host not in allowlist," confirmed and documented in ADR-0014). GitHub
# Actions runners and a normal developer machine both have real internet
# access, so this is exactly where this script is meant to run -- it was
# NOT run successfully by whoever wrote it, only written and reasoned
# about; this is the honest split, same as the Kafka mock-vs-real-broker
# and DynamoDB fake-transport-vs-live-endpoint splits elsewhere in this
# project.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50556}"
LOG_DIR="$(mktemp -d)"

# Los Angeles -- arbitrary but real coordinates, chosen only because
# they're a place with genuinely variable weather worth fetching.
"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" \
  --weather-lat=34.05 --weather-lon=-118.24 --weather-refresh-s=5 \
  >"$LOG_DIR/gateway.log" 2>&1 &
GW_PID=$!
cleanup() { kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT

# Give the background weather thread a real chance to complete an actual
# HTTPS round trip before sending any detections.
sleep 8

timeout 10 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-weather-live \
  --state-dir="$(mktemp -d)" --rate-hz=5 --duration-s=3 >"$LOG_DIR/agent.log" 2>&1

sleep 0.5
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true

echo "---- gateway ----"; cat "$LOG_DIR/gateway.log"

if ! grep -q "weather updated: temp=" "$LOG_DIR/gateway.log"; then
  echo "FAIL: no successful live weather fetch observed -- either the API is unreachable from this environment,"
  echo "      or the response shape has changed since ADR-0014 was written. Either way, this needs a human to look."
  exit 1
fi

if ! grep -q "cam-weather-live disconnected: received=15" "$LOG_DIR/gateway.log"; then
  echo "FAIL: detections should still have been delivered normally alongside weather fusion"; exit 1
fi

echo "PASS: live weather fetch succeeded and detections were fused and delivered normally"
