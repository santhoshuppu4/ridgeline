#!/usr/bin/env bash
# End-to-end test: agent -> gateway -> Kafka, all real, no Docker required.
#
# Uses ridgeline_mock_kafka_broker (librdkafka's real in-process mock
# cluster, real wire protocol) so this runs anywhere the project builds.
# For a production-realistic run against actual Redpanda/Kafka instead, use
# deploy/docker-compose.yml and pass its broker address to --kafka-brokers
# below -- the gateway code path is identical either way.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50594}"
LOG_DIR="$(mktemp -d)"

MOCK_PID=""; GW_PID=""
cleanup() { [[ -n "$MOCK_PID" ]] && kill "$MOCK_PID" 2>/dev/null || true; [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT

"$BUILD_DIR/tools/ridgeline_mock_kafka_broker" >"$LOG_DIR/mock_kafka.log" 2>&1 & MOCK_PID=$!
for _ in $(seq 1 50); do
  BOOTSTRAP="$(grep -m1 '^BOOTSTRAP=' "$LOG_DIR/mock_kafka.log" 2>/dev/null | cut -d= -f2 || true)"
  [[ -n "$BOOTSTRAP" ]] && break
  sleep 0.1
done
if [[ -z "${BOOTSTRAP:-}" ]]; then
  echo "FAIL: mock Kafka broker never printed its bootstrap address"; cat "$LOG_DIR/mock_kafka.log"; exit 1
fi
echo "mock Kafka bootstrap: $BOOTSTRAP"

"$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" --kafka-brokers="$BOOTSTRAP" \
  --kafka-topic="detections-smoke-test" --log-events >"$LOG_DIR/gateway.log" 2>&1 & GW_PID=$!
sleep 0.5
if ! grep -q "Kafka publish enabled" "$LOG_DIR/gateway.log"; then
  echo "FAIL: gateway did not report Kafka publish enabled"; cat "$LOG_DIR/gateway.log"; exit 1
fi

"$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-kafka-smoke \
  --state-dir="$(mktemp -d)" --rate-hz=20 --duration-s=3 >"$LOG_DIR/agent.log" 2>&1
AGENT_STATUS=$?

sleep 0.3
kill -INT "$GW_PID"; GW_STATUS=0; wait "$GW_PID" || GW_STATUS=$?; GW_PID=""

echo "---- agent ----"; cat "$LOG_DIR/agent.log"
echo "---- gateway ----"; cat "$LOG_DIR/gateway.log"

[[ $AGENT_STATUS -ne 0 ]] && { echo "FAIL: agent exited $AGENT_STATUS"; exit 1; }
[[ $GW_STATUS -ne 0 ]] && { echo "FAIL: gateway exited $GW_STATUS"; exit 1; }

RECEIVED="$(grep -oP 'received=\K[0-9]+' "$LOG_DIR/gateway.log" | tail -1)"
PUBLISHED="$(grep -oP 'kafka_published=\K[0-9]+' "$LOG_DIR/gateway.log" | tail -1)"
FAILED="$(grep -oP 'kafka_failed=\K[0-9]+' "$LOG_DIR/gateway.log" | tail -1)"

if [[ -z "$RECEIVED" || -z "$PUBLISHED" ]]; then
  echo "FAIL: could not parse received/kafka_published from gateway log"; exit 1
fi
if [[ "$RECEIVED" != "$PUBLISHED" ]]; then
  echo "FAIL: received=$RECEIVED but kafka_published=$PUBLISHED -- every acked event must have been published"
  exit 1
fi
if [[ "${FAILED:-0}" != "0" ]]; then
  echo "FAIL: kafka_failed=$FAILED (expected 0 against a healthy mock broker)"; exit 1
fi
echo "PASS: agent -> gateway -> Kafka, $PUBLISHED/$RECEIVED events published before ack, 0 failures"
