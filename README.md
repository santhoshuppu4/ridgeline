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
- [x] **Phase 1b-i** — K-of-N confirmation, detector interface, synthetic capture pipeline
- [x] **Phase 1b-ii** — OpenCV video capture, ONNX Runtime CPU inference (YOLOX-nano, COCO classes), zero-copy frame ring, `ridgeline_edge` tool
- [x] **Phase 1b-iii** — agent `--video` mode: real pipeline -> gRPC, shared `EdgePipeline` component
- [x] **Phase 1c** — write-ahead log, crash replay, kill -9 chaos test with identity oracle, fuzzed record parser
- [x] **Phase 1d-i** — Kafka event backbone: gateway publishes before ack, tested against librdkafka's real mock protocol, standalone mock-broker tool for Docker-free dev
- [x] **Phase 1d-ii** — Redis hot state (real redis-server tested), DynamoDB device shadow (hand-rolled SigV4, optimistic concurrency, fake-transport tested)
- [x] **Phase 2** — fleet device simulator (thread-per-connection measured: 161 threads/150 devices; a real reconnect-vs-gap-tracking bug found and documented)
- [ ] **Phase 3** — mTLS device identity, multi-tenancy, rate limiting, signed OTA
- [ ] **Phase 4** — weather fusion, alert engine, Terraform

## Phase 1b-ii: real inference (optional build)

```bash
sudo apt-get install -y libopencv-dev
./scripts/fetch-phase1b-assets.sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIDGELINE_WITH_ONNX=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/tools/ridgeline_infer_bench --threads=1
./build/tools/ridgeline_make_sample_video
./build/tools/ridgeline_edge --video=third_party/testdata/sample.avi --classes=16
```

The stock model detects COCO classes (person, car, dog, ...), **not smoke**.
See `context/adr/0005-onnx-runtime-cpu-inference.md`.

## Phase 1b-iii / 1c: durable delivery

```bash
./scripts/smoke_test.sh build          # gateway outage: 0 lost, 0 duplicates
./scripts/chaos_test.sh build          # kill -9 agent holding WAL-only events: all delivered
./build/tools/ridgeline_make_sample_video
./scripts/smoke_test_video.sh build    # real video -> ONNX -> gRPC: exactly 4 events (needs -DRIDGELINE_WITH_ONNX=ON)
```

See `context/adr/0006-agent-write-ahead-log.md`.

## Phase 1d-i: Kafka event backbone

```bash
sudo apt-get install -y librdkafka-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIDGELINE_WITH_KAFKA=ON
cmake --build build
ctest --test-dir build --output-on-failure     # runs against librdkafka's real in-process mock cluster
./scripts/kafka_gateway_smoke_test.sh build    # real agent -> gateway -> Kafka, no Docker required
```

For a production-realistic run against actual Redpanda instead of the mock cluster:

```bash
docker compose -f deploy/docker-compose.yml up -d
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 --kafka-brokers=localhost:19092
```

See `context/adr/0007-kafka-event-backbone.md`, including a real use-after-free
bug found and fixed while building this.

## Phase 1d-ii: Redis hot state + DynamoDB device shadow

```bash
sudo apt-get install -y redis-server libhiredis-dev libcurl4-openssl-dev nlohmann-json3-dev
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRIDGELINE_WITH_REDIS=ON -DRIDGELINE_WITH_DYNAMODB=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Redis tests run against a real `redis-server` subprocess. DynamoDB tests
verify SigV4 signing (cross-checked against an independent Python
implementation) and request/response logic via an injected fake transport
-- no live DynamoDB needed for those. To verify against a real endpoint:

```bash
docker compose -f deploy/docker-compose.yml up -d   # brings up DynamoDB Local too
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 \
  --redis-host=localhost --redis-port=6379 \
  --dynamodb-endpoint=http://localhost:8000 --dynamodb-region=us-west-2 \
  --dynamodb-access-key=local --dynamodb-secret-key=local
```

See `context/adr/0008-hot-state-and-device-shadow.md`, including a real
gateway-availability bug found and fixed (Redis being down at startup used
to take down the whole gateway, contradicting its own "best-effort"
design).

## Phase 2: fleet device simulator

```bash
cmake --build build --target ridgeline_device_simulator
./build/gateway/ridgeline_gateway --listen=127.0.0.1:50051 &
./build/tools/ridgeline_device_simulator --gateway=127.0.0.1:50051 --devices=500 --duration-s=30 --rate-hz=2 --disconnect-every-s=15
```

`scripts/fleet_smoke_test.sh` runs a small, deterministic correctness check
in CI (10 devices, no fault injection, every sent event must be acked). Real
throughput/latency/thread-count numbers need real multi-core hardware --
this project's own sandbox has one CPU core, which is exactly why those
numbers aren't reported from CI. See `context/adr/0009-fleet-device-simulator.md`
for a real design gap found (the gateway couldn't distinguish a device
reconnecting without WAL-backed resume state from one that actually lost
events -- confirmed on real hardware: 500 devices, 100% acked, yet false
`lost=` on nearly every reconnect) and `context/adr/0010-durable-resume-flag.md`
for the fix, verified in both directions with a dedicated protocol-level test.

## Honesty notes

Performance numbers live in `BENCHMARKS.md` with hardware, commit, and
method. Nothing here claims fleet scale — the device simulator that would
justify that doesn't exist yet.
