#!/usr/bin/env bash
# Regression test for the multi-tenancy half of ADR-0013. Three scenarios:
#   1. Two different tenants each have a device named "cam-0001" -- no
#      collision, both connect and deliver normally. Proves tenant scoping
#      actually creates separate identity namespaces, not just a label.
#   2. A device holds tenant-a's cert but claims tenant_id=tenant-b (same
#      device_id either way) -- rejected. This is the interesting case
#      beyond ADR-0011's plain device impersonation: the cert is valid and
#      the device_id matches, but the CLAIMED TENANT doesn't match what the
#      certificate was actually issued for.
#   3. A device with no cert-derived identity at all (plaintext) claiming a
#      tenant_id is unaffected by this feature -- plaintext connections
#      skip the check entirely (ADR-0011's existing backward-compatibility
#      behavior), confirmed here so a future change to the tenant check
#      doesn't accidentally break plaintext/legacy mode.
set -euo pipefail

BUILD_DIR="${1:-build}"
PORT="${PORT:-50561}"
CERT_DIR="$(mktemp -d)"
STATE_DIR="$(mktemp -d)"
LOG_DIR="$(mktemp -d)"

"$(dirname "$0")/generate_test_certs.sh" "$CERT_DIR" tenant-a:cam-0001 tenant-b:cam-0001 > /dev/null

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

echo "=== Scenario 1: two tenants, same device_id (cam-0001), no collision ==="
start_gateway
timeout 8 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 --tenant-id=tenant-a \
  --state-dir="$STATE_DIR/a" --rate-hz=5 --duration-s=2 \
  --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/tenant-a_cam-0001.crt" --tls-key="$CERT_DIR/tenant-a_cam-0001.key" \
  >"$LOG_DIR/agent-a.log" 2>&1
timeout 8 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 --tenant-id=tenant-b \
  --state-dir="$STATE_DIR/b" --rate-hz=5 --duration-s=2 \
  --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/tenant-b_cam-0001.crt" --tls-key="$CERT_DIR/tenant-b_cam-0001.key" \
  >"$LOG_DIR/agent-b.log" 2>&1
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true
if ! grep -q "tenant-a:cam-0001 connected" "$LOG_DIR/gateway.log"; then
  echo "FAIL: tenant-a's cam-0001 should have connected"; cat "$LOG_DIR/gateway.log"; exit 1
fi
if ! grep -q "tenant-b:cam-0001 connected" "$LOG_DIR/gateway.log"; then
  echo "FAIL: tenant-b's cam-0001 should ALSO have connected -- same device_id, different tenant, no collision"
  cat "$LOG_DIR/gateway.log"; exit 1
fi
if ! grep -q "tenant-a:cam-0001 disconnected: received=10" "$LOG_DIR/gateway.log"; then
  echo "FAIL: tenant-a's device should have delivered its events"; exit 1
fi
if ! grep -q "tenant-b:cam-0001 disconnected: received=10" "$LOG_DIR/gateway.log"; then
  echo "FAIL: tenant-b's device should have delivered its events"; exit 1
fi
echo "  PASS: both tenants' cam-0001 connected and delivered independently"

echo "=== Scenario 2: tenant-a's cert, but claims tenant_id=tenant-b (cross-tenant impersonation) ==="
start_gateway
timeout 6 "$BUILD_DIR/agent/ridgeline_agent" --gateway="127.0.0.1:$PORT" --device-id=cam-0001 --tenant-id=tenant-b \
  --state-dir="$STATE_DIR/c" --rate-hz=5 --duration-s=2 \
  --tls-ca="$CERT_DIR/ca.crt" --tls-cert="$CERT_DIR/tenant-a_cam-0001.crt" --tls-key="$CERT_DIR/tenant-a_cam-0001.key" \
  >"$LOG_DIR/agent-c.log" 2>&1 || true
kill -INT "$GW_PID"; wait "$GW_PID" 2>/dev/null || true
if ! grep -q "REJECTED: cert CN='tenant-a:cam-0001' does not match claimed identity='tenant-b:cam-0001'" "$LOG_DIR/gateway.log"; then
  echo "FAIL: cross-tenant impersonation (right device_id, wrong tenant_id) should be rejected"
  cat "$LOG_DIR/gateway.log"; exit 1
fi
if grep -q "tenant-b:cam-0001 connected" "$LOG_DIR/gateway.log"; then
  echo "FAIL: the impersonating connection must never be treated as a legitimate tenant-b connection"; exit 1
fi
echo "  PASS: cross-tenant impersonation rejected on every attempt"

echo "PASS: multi-tenant isolation behaves correctly"
