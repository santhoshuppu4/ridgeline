#!/usr/bin/env bash
# Regression test for ADR-0011 (mTLS device identity). Three scenarios:
#   1. A device presents its own cert and claims its own device_id -> works.
#   2. A device presents device B's cert but claims to be device A
#      (impersonation) -> rejected, every retry, zero events delivered.
#   3. A device with no TLS at all connects to an mTLS-required gateway ->
#      the connection never completes a handshake.
# Testing all three matters: a fix that only handles #2 could still leave
# #3 open (e.g. an insecure fallback path), and a fix that's too strict
# could break #1 (rejecting legitimate devices).
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50567}"
CERT_DIR="$(mktemp -d)"
STATE_DIR="$(mktemp -d)"
LOG_DIR="$(mktemp -d)"

"$(dirname "$0")/generate_test_certs.sh" "$CERT_DIR" cam-0001 cam-0002 > /dev/null

GW_PID=""
cleanup() { [[ -n "$GW_PID" ]] && kill "$GW_PID" 2>/dev/null || true; }
trap cleanup EXIT

start_gateway() {
  "$BUILD_DIR/gateway/ridgeline_gateway" --listen="127.0.0.1:$PORT" \
    --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/server.crt" --tls-key="$CERT_DIR/server.key" \
    >"$LOG_DIR/gateway.log" 2>&1 &
  GW_PID=$!
  sleep 0.4
}

echo "=== Scenario 1: matching identity ==="
start_gateway
timeout 10 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 \
  --state-dir="$STATE_DIR/s1" --rate-hz=5 --duration-s=2 \
  --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/cam-0001.crt" --tls-key="$CERT_DIR/cam-0001.key" \
  >"$LOG_DIR/agent1.log" 2>&1
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true
if ! grep -q "cam-0001 disconnected: received=10 " "$LOG_DIR/gateway.log"; then
  echo "FAIL: matching identity should deliver all 10 events"; cat "$LOG_DIR/gateway.log"; exit 1
fi
echo "  PASS: matching identity delivered events normally"

echo "=== Scenario 2: impersonation (cam-0002's cert, claims cam-0001) ==="
start_gateway
timeout 6 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 \
  --state-dir="$STATE_DIR/s2" --rate-hz=5 --duration-s=2 \
  --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/cam-0002.crt" --tls-key="$CERT_DIR/cam-0002.key" \
  >"$LOG_DIR/agent2.log" 2>&1 || true
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true
if ! grep -q "REJECTED: cert CN='cam-0002' does not match claimed device_id='cam-0001'" "$LOG_DIR/gateway.log"; then
  echo "FAIL: impersonation attempt should be rejected"; cat "$LOG_DIR/gateway.log"; exit 1
fi
if grep -q "cam-0001 connected" "$LOG_DIR/gateway.log"; then
  echo "FAIL: an impersonating connection must never be treated as legitimately connected"; exit 1
fi
echo "  PASS: impersonation rejected on every attempt, zero legitimate connections"

echo "=== Scenario 3: plaintext agent against an mTLS-required gateway ==="
start_gateway
timeout 5 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 \
  --state-dir="$STATE_DIR/s3" --rate-hz=5 --duration-s=2 \
  >"$LOG_DIR/agent3.log" 2>&1 || true
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true
if grep -q "connected (resume" "$LOG_DIR/gateway.log"; then
  echo "FAIL: a plaintext connection must never reach the application layer of an mTLS-required gateway"; exit 1
fi
echo "  PASS: plaintext connection never completed a handshake"

echo "PASS: all three mTLS scenarios behave correctly"
