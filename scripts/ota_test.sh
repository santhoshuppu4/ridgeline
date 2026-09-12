#!/usr/bin/env bash
# End-to-end regression test for ridgeline_ota_tool: generate a real
# keypair, sign a real manifest, verify it succeeds, then tamper with the
# on-disk manifest file directly (not the in-memory object -- this is what
# an attacker who intercepted the file would actually be able to do) and
# confirm verification fails.
set -euo pipefail

BUILD_DIR="${1:-build}"
DIR="$(mktemp -d)"

"$BUILD_DIR/tools/ridgeline_ota_tool" genkey --out-prefix="$DIR/keys"
[[ -s "$DIR/keys.private.pem" && -s "$DIR/keys.public.pem" ]] || { echo "FAIL: keys not written"; exit 1; }

FAKE_SHA="$(echo -n 'ridgeline-ota-regression-test-binary' | sha256sum | cut -d' ' -f1)"
"$BUILD_DIR/tools/ridgeline_ota_tool" sign --private-key="$DIR/keys.private.pem" \
  --version=7 --binary-sha256="$FAKE_SHA" --url="https://updates.example.com/agent-v7.bin" \
  --out="$DIR/manifest.signed"

if ! "$BUILD_DIR/tools/ridgeline_ota_tool" verify --public-key="$DIR/keys.public.pem" --manifest="$DIR/manifest.signed"; then
  echo "FAIL: a freshly signed, untouched manifest should verify"; exit 1
fi
echo "PASS: freshly signed manifest verifies"

sed -i 's/^version=7$/version=999/' "$DIR/manifest.signed"
if "$BUILD_DIR/tools/ridgeline_ota_tool" verify --public-key="$DIR/keys.public.pem" --manifest="$DIR/manifest.signed"; then
  echo "FAIL: a tampered on-disk manifest must not verify"; exit 1
fi
echo "PASS: tampered manifest correctly rejected"

# A manifest signed by a DIFFERENT key must also fail against this public key.
"$BUILD_DIR/tools/ridgeline_ota_tool" genkey --out-prefix="$DIR/other-keys"
"$BUILD_DIR/tools/ridgeline_ota_tool" sign --private-key="$DIR/other-keys.private.pem" \
  --version=7 --binary-sha256="$FAKE_SHA" --url="https://updates.example.com/agent-v7.bin" \
  --out="$DIR/manifest-wrong-signer.signed"
if "$BUILD_DIR/tools/ridgeline_ota_tool" verify --public-key="$DIR/keys.public.pem" --manifest="$DIR/manifest-wrong-signer.signed"; then
  echo "FAIL: a manifest signed by a different key must not verify against this public key"; exit 1
fi
echo "PASS: manifest signed by a different key correctly rejected"

echo "PASS: OTA sign/verify/tamper/wrong-signer all behave correctly"
