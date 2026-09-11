# ADR-0007: Kafka event backbone, and making "ack means durable" literal

- **Status:** Accepted
- **Date:** 2026-09-11

## Context
ADR-0001 said "an ack means durably stored," but nothing backed that beyond
the gateway process's own memory. Phase 1d-i adds a real Kafka publish step
between validating a detection and acking it.

## Decisions

**Synchronous produce-then-ack.** `KafkaProducer::PublishSync()` blocks until
the specific message's delivery report is confirmed (or times out), and the
gateway only sends its `Ack` after that returns true. If Kafka is
unreachable, the gateway never acks -- the agent's WAL-backed resend
(ADR-0006) retries exactly as it would after a dropped connection. This
trades producer throughput for a durability story simple enough to state and
test in one sentence: verified directly by pointing the gateway at an
unreachable broker and confirming 80/80 sent events stayed unacked with
`kafka_failed=80`, not silently dropped or falsely acked.

**Partitioned by device_id.** The original design calls for
`tenant_id:device_id` once multi-tenancy exists (Phase 3); device_id alone
preserves per-device ordering until then, which is the property a downstream
consumer replaying one camera's history actually needs.

**Replayed (duplicate) events are not republished to Kafka.** The gateway
already durably has them; republishing on every gRPC-level retry would turn
network flakiness into duplicate Kafka records.

## A real bug found while building this

The first version put the per-message delivery-tracking struct
(`PendingDelivery`) on `PublishSync()`'s own stack and passed a raw pointer to
it via librdkafka's `msg_opaque`. That's safe only if the delivery callback
always fires before `PublishSync()` gives up and returns. It doesn't, when a
broker is genuinely unreachable: librdkafka's connection-retry behavior took
far longer to internally fail a message than `message.timeout.ms` implied,
so the stack frame was gone by the time the callback actually ran and wrote
into it -- a use-after-free. The symptom in testing was a 90+ second hang
in `UnreachableBrokerFailsRatherThanHangingForever`, not a crash: the freed
stack memory happened to still be valid in that run. **Fix:** the struct is
now heap-allocated and reference-counted via `shared_ptr`. `PublishSync`
keeps one reference for as long as it's waiting; a second reference travels
through librdkafka via `msg_opaque`. Whichever side finishes last frees the
object. A local timeout can now return promptly without ever touching memory
the eventual (much later) delivery callback still needs.

## Testing without Docker

librdkafka ships `rdkafka_mock.h`: an in-process mock cluster speaking the
real Kafka wire protocol, not a hand-rolled fake. `tests/kafka/` runs against
that. `tools/mock_kafka_broker.cc` wraps the same mock cluster as a
standalone process (prints its bootstrap address, blocks until signaled), so
`scripts/kafka_gateway_smoke_test.sh` can run the ENTIRE agent -> gateway ->
Kafka path with no external services -- verified: 60/60 events published
before ack, 0 failures, clean under ASan+UBSan.

**What this does and doesn't prove.** The mock cluster validates protocol
correctness, the produce/ack/timeout logic, and the shared_ptr lifetime fix
-- genuinely, not superficially. It does NOT validate: multi-broker
rebalancing, real network partitions, disk-backed durability semantics, or
performance under real network latency. Before trusting this in anything
resembling production, run the same `ridgeline_gateway --kafka-brokers=...`
against the real Redpanda in `deploy/docker-compose.yml` (`docker compose -f
deploy/docker-compose.yml up -d`, then point `--kafka-brokers` at
`localhost:19092`) -- the gateway code path is identical either way; only the
broker underneath changes.

## Consequences
- `RIDGELINE_WITH_KAFKA` excluded from TSan for the same reason ONNX Runtime
  is (ADR-0005): the apt-packaged librdkafka is uninstrumented.
- Kafka topic/partition provisioning (`detections.v1`, replica count, retention)
  isn't handled by this code -- assumed pre-created for now; Terraform/infra
  automation for that is a later phase.
- `acks=all` plus a synchronous produce path bounds gateway throughput to
  Kafka's per-message round-trip latency. Fine at current event rates;
  revisit (batch produce, decouple ack from Kafka confirmation with WAL as
  the interim durability source) if `bench/` numbers against a real cluster
  show it's a bottleneck.
