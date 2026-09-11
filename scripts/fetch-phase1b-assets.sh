#!/usr/bin/env bash
# Downloads Phase 1b-ii third-party assets into third_party/ (gitignored) and
# verifies each against a pinned SHA-256 checksum.
#
# Why checksums: a download that silently changes (a re-tagged release, a
# truncated transfer, a compromised mirror) should fail loudly here, not show
# up weeks later as "the detector got worse" or "the build broke on CI."
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
mkdir -p "$TP/models" "$TP/testdata"

ORT_VERSION="1.23.2"
ORT_NAME="onnxruntime-linux-x64-${ORT_VERSION}"
ORT_URL="https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/${ORT_NAME}.tgz"
ORT_SHA256="1fa4dcaef22f6f7d5cd81b28c2800414350c10116f5fdd46a2160082551c5f9b"

MODEL_URL="https://github.com/Megvii-BaseDetection/YOLOX/releases/download/0.1.1rc0/yolox_nano.onnx"
MODEL_SHA256="c789161ed43c8269fcd4e67c67eeeb4e80c622da2eb296a20bc6007bd18a0b7d"

IMAGE_URL="https://raw.githubusercontent.com/Megvii-BaseDetection/YOLOX/main/assets/dog.jpg"
IMAGE_SHA256="5a9522051c3cec2bbd2f6323fccba32e8fbf3ddcc2b3e2fd46b04c720bc6f866"

fetch() {  # fetch URL DEST SHA256
  local url="$1" dest="$2" sha="$3"
  if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum --check --status; then
    echo "ok (cached)  $(basename "$dest")"
    return
  fi
  echo "downloading  $url"
  curl -fL --retry 3 --max-time 300 -o "$dest.partial" "$url"
  if ! echo "$sha  $dest.partial" | sha256sum --check --status; then
    echo "CHECKSUM MISMATCH for $url" >&2
    echo "  expected: $sha" >&2
    echo "  actual:   $(sha256sum "$dest.partial" | cut -d' ' -f1)" >&2
    rm -f "$dest.partial"
    exit 1
  fi
  mv "$dest.partial" "$dest"
  echo "ok (verified) $(basename "$dest")"
}

fetch "$ORT_URL" "$TP/${ORT_NAME}.tgz" "$ORT_SHA256"
if [[ ! -f "$TP/${ORT_NAME}/lib/libonnxruntime.so" ]]; then
  tar -xzf "$TP/${ORT_NAME}.tgz" -C "$TP"
  echo "extracted    $TP/${ORT_NAME}"
fi
fetch "$MODEL_URL" "$TP/models/yolox_nano.onnx" "$MODEL_SHA256"
fetch "$IMAGE_URL" "$TP/testdata/dog.jpg" "$IMAGE_SHA256"

echo
echo "All Phase 1b-ii assets present and verified under $TP"
