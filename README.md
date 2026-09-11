# Ridgeline

A distributed edge-vision control plane for wildfire-detection camera fleets.
A C++ agent detects smoke on-device and streams only events. A cloud control
plane manages the fleet: configuration, updates, identity, and alerting.

> **Status: Phase 0 networking + Phase 1 ring buffer, built, not yet wired together.**
> The agent still streams *fake* detections over gRPC (Phase 0). The
> lock-free SPSC ring buffer and arena-backed `Frame` type (Phase 1) exist
> and are fully tested, but the agent doesn't use them yet — see
> `context/adr/0002` for how to study them and wire them in yourself.

## Quickstart (WSL2 Ubuntu 24.04)

Clone under your Linux home directory, **not** `/mnt/c/...`.

```bash
./scripts/install-deps.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/smoke_test.sh build
```

### Sanitizer builds

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRIDGELINE_SANITIZE=address
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DRIDGELINE_SANITIZE=thread -DRIDGELINE_BUILD_NET=OFF
```

If TSan aborts with "unexpected memory mapping" under WSL2: `sudo sysctl vm.mmap_rnd_bits=28`.

## Layout

```
proto/       Protobuf contracts (agent <-> gateway)
core/        ring_buffer.h, frame.h, backoff.h — no gRPC dependency, runs under every sanitizer
agent/       Edge agent (Phase 0 networking; not yet using the ring buffer)
gateway/     Ingest gateway
tests/       GoogleTest unit + concurrency stress tests
scripts/     Dependency install and end-to-end smoke test
deploy/      docker-compose for local Kafka (Redpanda), DynamoDB Local, Redis
context/adr/ Architecture decision records, including the ring buffer study guide (0002)
```

## Start here if you're studying this repo

Read `context/adr/0002-spsc-ring-buffer-study-guide.md` first. It has the
reading order, three real bugs this implementation hit (a stack overflow, a
test logic bug, and a data race caught by ThreadSanitizer), and the
questions to be able to answer from memory before calling this "known."

## Roadmap

- [x] **Phase 0** — contracts, agent/gateway streaming, backoff, sanitizer CI, smoke test
- [x] **Phase 1a** — lock-free SPSC ring buffer + arena-backed Frame, tested under ASan/UBSan/TSan
- [ ] **Phase 1b** — wire the ring buffer into the agent: real capture thread, ONNX Runtime CPU inference, K-of-N confirmation, WAL + replay
- [ ] **Phase 1c** — Kafka, DynamoDB shadow, Redis
- [ ] **Phase 2** — device simulator, config reconciliation, benchmarks
- [ ] **Phase 3** — mTLS device identity, multi-tenancy, rate limiting, signed OTA
- [ ] **Phase 4** — weather fusion, alert engine, Terraform

## Honesty notes

Performance numbers live in `BENCHMARKS.md` with hardware, commit, and
method. Nothing here claims fleet scale — the device simulator that would
justify that doesn't exist yet.
