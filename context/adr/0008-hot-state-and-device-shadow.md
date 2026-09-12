# ADR-0008: Redis hot state and DynamoDB device shadow

- **Status:** Accepted
- **Date:** 2026-09-11

## Context
Phase 1d-ii adds the two remaining pieces from the original event-backbone
plan: Redis for fast, ephemeral per-device state, and DynamoDB for the
durable "device shadow" (reported config). Both are fed from Heartbeat
messages, orthogonal to Kafka's detection-event durability path.

## Decisions

**Redis is a cache, not a source of truth.** One HASH per device
(`device:<id>`: `last_seen_unix_ns`, `queue_depth`, `frames_dropped`). If
lost (restart, eviction), nothing important is gone -- the next heartbeat
rebuilds it. This is why `RedisHotStateStore::Update()` returns bool
instead of throwing: a failed write is logged, not fatal.

**DynamoDB shadow uses optimistic concurrency via a version attribute.**
`PutReported` takes an `expected_version` and issues a conditional
`PutItem` (`version = :v`, or `attribute_not_exists(device_id)` for
version 0 / first write). A conflict returns `kVersionConflict`, distinct
from `kError`, so a caller can re-read and retry rather than silently
losing a concurrent write. The gateway's `UpsertReportedWithRetry` uses
exactly this: read current version, write, retry up to 3 times on conflict.

**SigV4 signing is hand-rolled, not via the AWS SDK.** Small, dependency-light,
and -- more importantly -- an unambiguous, fully specified algorithm that can
be verified independently of any AWS account or live endpoint. Verified two
ways: (1) an independently-written Python reference implementation
(hashlib/hmac directly) produces a byte-identical signature for a fixed
input, which is real cross-implementation evidence, not a test checking code
against itself; (2) sensitivity tests confirm changing the body, timestamp,
secret key, or any signed header changes the output.

**DeviceShadowStore depends on an `HttpTransport` interface, not libcurl
directly.** The retry/error-classification logic (throttling vs. conditional
check failure vs. genuine network error) is fully unit-tested with an
injected `FakeHttpTransport` returning canned responses -- zero network,
zero DynamoDB, zero flakiness. `CurlHttpTransport` (the real implementation)
is exercised only by hitting a live endpoint, which this project's sandbox
cannot do at all (no AWS network access, no Docker) -- see "What this proves
and doesn't" below.

## A real design bug found and fixed

`RedisHotStateStore`'s constructor throws if the initial connection fails
-- deliberately, matching Kafka/DynamoDB's "fail loudly at construction"
pattern for a malformed config. But wiring that directly into the gateway
meant: if Redis happened to be down when the GATEWAY started, the gateway
refused to start at all, taking the entire detection pipeline down over an
explicitly-documented-as-best-effort dependency. Caught by testing the
failure path directly (pointing the gateway at an unreachable Redis and
checking whether the process was still alive) rather than only testing the
happy path. Fixed: the gateway catches the constructor's exception, logs a
warning, and proceeds with `integrations.redis == nullptr` -- the same
"missing optional integration" shape Kafka and DynamoDB already have when
not configured at all.

## Verification

- Redis: 6 tests against a REAL `redis-server` subprocess (not a mock --
  it's a lightweight apt package, no reason to test less than the real
  thing), plus a real end-to-end run (agent heartbeats -> gateway -> Redis,
  confirmed via `redis-cli HGETALL`).
- DynamoDB: 7 SigV4 tests (cross-check + 5 sensitivity variants) and 9
  DeviceShadowStore tests via `FakeHttpTransport`, including a mutation
  test (disabling `ConditionalCheckFailedException` detection correctly
  fails the version-conflict test).
- All of the above clean under ASan+UBSan.
- Gateway-level fail-safe verified directly: unreachable Redis at startup
  no longer prevents the gateway process from starting.

## What this does and doesn't prove
Genuinely verified: the SigV4 algorithm, the DynamoDB request/response
shape and error classification, the Redis wire protocol against a real
server, and the gateway's fail-safe behavior. NOT verified in this
project's sandbox (no AWS/Docker network access here): a live round trip
against DynamoDB Local or real AWS DynamoDB. That step needs a real
machine -- point `--dynamodb-endpoint` at the `deploy/docker-compose.yml`
DynamoDB Local container (`http://localhost:8000`) and confirm a
`GetReported`/`PutReported` round trip actually works end to end there,
the same way the Kafka mock-vs-real split was closed in ADR-0007.

## Consequences
- `RIDGELINE_WITH_DYNAMODB` excluded from TSan (libcurl's system
  dependencies are uninstrumented), same reasoning as ONNX/Kafka.
- DynamoDB table (`device_shadows`) and its schema aren't provisioned by
  this code -- assumed pre-created; Terraform for that is a later phase.
- Redis reconnect is manual (`Reconnect()`), not automatic on failure --
  acceptable at current call volume; revisit if sustained Redis outages
  in practice show this needs to self-heal.
