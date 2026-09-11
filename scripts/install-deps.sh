#!/usr/bin/env bash
set -euo pipefail
if [[ "$(pwd)" == /mnt/* ]]; then
  echo "WARNING: this repo is on the Windows filesystem ($(pwd)). Clone it under your Linux home instead."
fi
SUDO=""
[[ $EUID -ne 0 ]] && SUDO="sudo"
$SUDO apt-get update
$SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
  build-essential cmake ninja-build pkg-config git curl gdb clang-format \
  libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler libgtest-dev
