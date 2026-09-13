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
- [x] **Phase 3-i** — mTLS device identity: cert CN cross-checked against claimed device_id, verified against real impersonation attempts; found and fixed an unrelated fake-detection rate-loop bug along the way
- [x] **Phase 4** — config reconciliation (real live rate/K-of-N changes, both agent modes) and signed OTA manifest trust (Ed25519, hand-rolled, 4 mutation tests); full binary distribution/A-B-swap/watchdog rollback scoped out explicitly
- [x] **Phase 3-ii** — multi-tenancy (tenant identity bound to cert CN, isolation verified both ways) and per-tenant rate limiting; found and fixed the same false-gap mistake as ADR-0010, this time within a single stream
- [x] **Phase 5-i** — weather fusion + alert engine: injectable-transport weather client (8 tests, no network needed), pure-function alert severity with mutation-tested escalation logic (14 tests), graceful degradation verified in a real running gateway against a genuinely unreachable API
- [x] **Phase 5-ii** — Terraform for AWS (VPC, DynamoDB, ElastiCache, MSK Serverless, S3, ECS Fargate); validated with terraform-config-inspect + manual cross-reference (real terraform binary unreachable from this sandbox, never applied against real AWS); found a real gap -- KafkaProducer has no IAM SASL support needed for MSK Serverless

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

## Phase 3-i: mTLS device identity

```bash
./scripts/generate_test_certs.sh /tmp/ridgeline-certs cam-0001 cam-0002
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 \
  --tls-ca=/tmp/ridgeline-certs/ca.crt --tls-cert=/tmp/ridgeline-certs/server.crt --tls-key=/tmp/ridgeline-certs/server.key
./build/agent/ridgeline_agent --gateway=127.0.0.1:50051 --device-id=cam-0001 --state-dir=/tmp/cam1-state \
  --tls-ca=/tmp/ridgeline-certs/ca.crt --tls-cert=/tmp/ridgeline-certs/cam-0001.crt --tls-key=/tmp/ridgeline-certs/cam-0001.key
```

Without any `--tls-*` flags, both sides behave exactly as before (plaintext).
`scripts/mtls_test.sh` verifies matching identity, an impersonation attempt
(rejected), and a plaintext connection against an mTLS-required gateway
(handshake never completes). See `context/adr/0011-mtls-device-identity.md`,
including an unrelated rate-loop bug found and fixed along the way, and a
known (documented, not yet fixed) issue with `--duration-s` being a soft
limit when a gateway is unreachable.

## Phase 4: config reconciliation + signed OTA manifest

```bash
cat > /tmp/device-configs.txt <<'EOC'
cam-0001,7,3,5,0.35,15
EOC
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 --device-configs=/tmp/device-configs.txt
./build/agent/ridgeline_agent --gateway=127.0.0.1:50051 --device-id=cam-0001 --state-dir=/tmp/cam1 --rate-hz=5

./build/tools/ridgeline_ota_tool genkey --out-prefix=/tmp/ota-keys
./build/tools/ridgeline_ota_tool sign --private-key=/tmp/ota-keys.private.pem --version=42 \
  --binary-sha256=$(sha256sum some-binary | cut -d' ' -f1) --url=https://example.com/agent-v42.bin --out=/tmp/manifest.signed
./build/tools/ridgeline_ota_tool verify --public-key=/tmp/ota-keys.public.pem --manifest=/tmp/manifest.signed
```

`scripts/config_reconciliation_test.sh` and `scripts/ota_test.sh` run the
real end-to-end checks. See `context/adr/0012-config-reconciliation-and-ota-manifest.md`
for exactly what signed OTA does and does not cover here -- binary
download, A/B partition swap, and watchdog rollback are explicitly
out of scope for this phase.

## Phase 3-ii: multi-tenancy + rate limiting

```bash
./scripts/generate_test_certs.sh /tmp/certs tenant-a:cam-0001 tenant-b:cam-0001
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 \
  --tls-ca=/tmp/certs/ca.crt --tls-cert=/tmp/certs/server.crt --tls-key=/tmp/certs/server.key \
  --rate-limit-capacity=50 --rate-limit-per-second=20
./build/agent/ridgeline_agent --gateway=127.0.0.1:50051 --device-id=cam-0001 --tenant-id=tenant-a --state-dir=/tmp/t \
  --tls-ca=/tmp/certs/ca.crt --tls-cert=/tmp/certs/tenant-a_cam-0001.crt --tls-key=/tmp/certs/tenant-a_cam-0001.key
```

`scripts/multi_tenancy_test.sh` and `scripts/rate_limit_test.sh` run the
real end-to-end checks. See `context/adr/0013-multi-tenancy-and-rate-limiting.md`,
including a false-"lost"-events bug found in the rate limiter -- the same
mistake ADR-0010 fixed for reconnects, this time inside a single stream.

## Phase 5-i: weather fusion + alert engine

```bash
./build/gateway/ridgeline_gateway --listen=0.0.0.0:50051 --weather-lat=34.05 --weather-lon=-118.24 --weather-refresh-s=300
```

Every confirmed detection gets fused with the most recent weather reading
into an `ALERT severity=...` log line. With no `--weather-lat`/`--weather-lon`,
behavior is unchanged (weather fusion is off by default). Missing or
unreachable weather data never suppresses an alert -- confirmed directly:
this project's own sandbox cannot reach the real weather API at all (a
direct curl returns "Host not in allowlist"), and running the gateway with
weather enabled there still produced correctly-tiered alerts from
confidence alone.

`scripts/weather_test.sh` exercises a REAL live round trip against
Open-Meteo -- it needs genuine internet access this sandbox doesn't have,
so it's written to run in CI (GitHub Actions runners have normal internet
access) or on your own machine, not here. See
`context/adr/0014-weather-fusion-and-alert-engine.md` for the full split
between what's verified here and what needs real infrastructure.

## Phase 5-ii: Terraform for AWS

```bash
cd terraform
terraform init
terraform plan -var="gateway_image=<your-ecr-image-uri>"
terraform apply -var="gateway_image=..."
```

See `terraform/README.md` for the full workflow (building/pushing the
gateway image first, finding the deployed gateway's address, tearing
down) and `context/adr/0015-terraform-aws-infrastructure.md` for what was
and wasn't verified, including a real integration gap: the existing
`KafkaProducer` has no AWS IAM SASL support, which MSK Serverless
requires -- named explicitly rather than left for whoever connects the
two first.

## Live demo

A real, running slice of this project -- the actual AlertEngine/WeatherClient
code, live in AWS Lambda, at zero guaranteed cost. See `demo/README.md`
for the full deploy workflow and `context/adr/0016-live-demo.md` for what
was and wasn't verified from this sandbox.

## Honesty notes

Performance numbers live in `BENCHMARKS.md` with hardware, commit, and
method. Nothing here claims fleet scale — the device simulator that would
justify that doesn't exist yet.
