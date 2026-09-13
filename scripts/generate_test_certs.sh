#!/usr/bin/env bash
# Generates a self-signed CA plus a server (gateway) certificate and one or
# more device (agent) certificates signed by it, all under a chosen output
# directory. FOR DEVELOPMENT/TESTING ONLY -- this is not how you'd run a
# real CA (no HSM, no revocation, a long-lived CA key sitting on disk in
# plaintext). See context/adr/0011 for what a production version would need.
#
# Each device certificate's Common Name (CN) encodes BOTH identities the
# gateway needs to verify cryptographically: "tenant_id:device_id" (ADR-0013
# extends ADR-0011's device-only identity check to cover tenant boundaries
# too). A device can no longer just SAY it belongs to tenant A -- it has to
# hold a cert whose CN says so.
#
# Usage:
#   ./scripts/generate_test_certs.sh /tmp/ridgeline-certs tenant-a:cam-0001 tenant-a:cam-0002 tenant-b:cam-0001
set -euo pipefail

OUT="${1:?usage: $0 <output-dir> [tenant_id:device_id ...]}"
shift
DEVICE_SPECS=("$@")

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

for spec in "${DEVICE_SPECS[@]}"; do
  # spec is "tenant_id:device_id" -- becomes the cert's CN verbatim, and a
  # filename-safe version (":" -> "_") for the key/cert files themselves.
  file_stem="${spec//:/_}"
  echo "== Device cert: $spec (files: ${file_stem}.key / .crt) =="
  openssl genrsa -out "${file_stem}.key" 2048 2>/dev/null
  openssl req -new -key "${file_stem}.key" -subj "/O=Ridgeline/CN=${spec}" -out "${file_stem}.csr"
  openssl x509 -req -in "${file_stem}.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
    -days 825 -sha256 -out "${file_stem}.crt" 2>/dev/null
  rm -f "${file_stem}.csr"
done

echo
echo "Certificates written to $OUT:"
ls -la "$OUT"/*.crt "$OUT"/*.key 2>/dev/null
