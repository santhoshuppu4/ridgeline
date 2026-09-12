#!/usr/bin/env bash
# Generates a self-signed CA plus a server (gateway) certificate and one or
# more device (agent) certificates signed by it, all under a chosen output
# directory. FOR DEVELOPMENT/TESTING ONLY -- this is not how you'd run a
# real CA (no HSM, no revocation, a long-lived CA key sitting on disk in
# plaintext). See context/adr/0011 for what a production version would need.
#
# Each device certificate's Common Name (CN) IS the device_id. That's the
# whole point: the gateway can cross-check the CN gRPC's mTLS handshake
# already cryptographically verified against the device_id the agent claims
# in its Hello message, so a device can no longer just SAY it's "cam-5" --
# it has to hold cam-5's private key.
#
# Usage:
#   ./scripts/generate_test_certs.sh /tmp/ridgeline-certs cam-0001 cam-0002 ...
set -euo pipefail

OUT="${1:?usage: $0 <output-dir> [device-id ...]}"
shift
DEVICE_IDS=("$@")

mkdir -p "$OUT"
cd "$OUT"

echo "== CA =="
openssl genrsa -out ca.key 4096 2>/dev/null
openssl req -x509 -new -nodes -key ca.key -sha256 -days 3650 \
  -subj "/O=Ridgeline Dev CA/CN=Ridgeline Dev Root CA" -out ca.crt

echo "== Server (gateway) cert =="
openssl genrsa -out server.key 2048 2>/dev/null
openssl req -new -key server.key -subj "/O=Ridgeline/CN=ridgeline-gateway" -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -days 825 -sha256 -extfile <(printf "subjectAltName=DNS:localhost,DNS:ridgeline-gateway,IP:127.0.0.1") \
  -out server.crt 2>/dev/null
rm -f server.csr

for device_id in "${DEVICE_IDS[@]}"; do
  echo "== Device cert: $device_id =="
  openssl genrsa -out "${device_id}.key" 2048 2>/dev/null
  openssl req -new -key "${device_id}.key" -subj "/O=Ridgeline/CN=${device_id}" -out "${device_id}.csr"
  openssl x509 -req -in "${device_id}.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days 825 -sha256 -out "${device_id}.crt" 2>/dev/null
  rm -f "${device_id}.csr"
done

echo
echo "Certificates written to $OUT:"
ls -la "$OUT"/*.crt "$OUT"/*.key 2>/dev/null
