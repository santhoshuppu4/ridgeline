# ADR-0013: Multi-tenancy and rate limiting

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
Phase 3-i (ADR-0011) gave the gateway a cryptographic way to verify a
device's claimed identity. This phase extends that to tenant boundaries,
and adds per-tenant rate limiting -- the remaining items from the original
"Phase 3" scope.

## Multi-tenancy

**Tenant identity is bound to the certificate the same way device identity
already was.** `generate_test_certs.sh` now mints certs with CN =
`tenant_id:device_id`. `Hello` gained a `tenant_id` field; the gateway
constructs the expected CN from whatever the agent claims
(`tenant_id:device_id`, or bare `device_id` for a legacy/single-tenant
cert) and compares it against the cert's actual CN -- extending ADR-0011's
check rather than adding a second, parallel mechanism.

**Verified two ways, not one:** two different tenants can each have a
device named `cam-0001` with zero collision (proving tenant scoping
creates real separate namespaces, not just a label), AND a device holding
tenant A's cert cannot claim to be tenant B while keeping the same
device_id (the interesting case beyond plain device impersonation: the
cert is valid, the device_id matches, only the claimed tenant is wrong).

## Rate limiting

**`TokenBucket`**: capacity + refill rate, deterministic via an injected
clock (no sleeping in tests -- an exact instantaneous time jump either
produces the exact expected token count or it doesn't). One bucket per
tenant, or per device_id when a connection has no tenant (so single-tenant
deployments still get protection). A rejected event gets no ack, exactly
the same "the agent's WAL-backed resend will retry" pattern already used
for Kafka publish failures.

## A real bug, structurally identical to ADR-0010's, found in a new place

Testing the rate limiter at a sustained rate above its refill rate (20Hz
against capacity=5, refill=2/sec) showed `lost=42` in the gateway's
disconnect summary despite zero real data loss -- every one of those 42
"missing" sequence numbers was a deliberate rate-limit rejection, not a
transport failure. This is ADR-0010's mistake again (conflating an
EXPLAINED gap with an unexplained one), just triggered within a single
continuous stream instead of across reconnects.

**Fix:** the gap counter (`gap_events`) still tracks every raw skip, but
the value actually logged as `lost=` subtracts the connection's own
`rate_limited` count, clamped at zero. Verified: the same 20Hz-vs-capacity=5
scenario now reports `lost=0`, with `received=10` and `rate_limited=50`
telling the true story. Note the clamp is doing real work here, not
padding: the raw gap sum (42) is actually LESS than rate_limited (50),
because trailing rejections after the last accepted detection never get
tallied into the raw gap number at all (nothing arrives afterward to
compute a jump against) -- those are still fully accounted for by
`rate_limited` alone, so subtracting-and-clamping is the correct behavior,
not a coincidence that happened to work once.

## A second real trigger of ADR-0011's known soft-limit issue

Writing `scripts/rate_limit_test.sh` without an external `timeout` around
the agent invocation reproduced the exact hang ADR-0011 documented: with
most events rate-limited (never acked), the agent's own `--duration-s`
never fires, since its exit condition requires the outbox to be empty.
Fixed the TEST (wrapped the agent call in `timeout 8`, consistent with
every other script in this project that exercises a can't-complete-
delivery scenario) rather than the agent -- this is the same known,
deliberately-not-yet-fixed issue, encountered for a second time, not a new
bug needing a new decision.

## Verification
- `TokenBucket`: 8 deterministic unit tests.
- `scripts/multi_tenancy_test.sh`: cross-tenant coexistence (no collision)
  and cross-tenant impersonation (rejected), both against a real mTLS
  gateway and real agents.
- `scripts/rate_limit_test.sh`: real throttling at real scale, `lost=0`
  attribution confirmed.
- Full existing suite (94 unit tests, 8 prior regression scripts)
  unaffected -- confirmed after two log-format bugs found and fixed along
  the way (a missing space in the connect log, and the disconnect log
  never having gained a tenant prefix at all).

## Consequences
- A rate-limited event that's never resent (the connection never drops)
  can, in principle, sit unacked indefinitely within one continuous
  stream -- the same underlying gap ADR-0011 named for `--duration-s`,
  now also relevant to rate limiting specifically. Not fixed here for the
  same reason: needs a deliberate design decision, not a reactive patch.
- Per-tenant token buckets are stored in an unbounded in-memory map with
  no eviction -- fine at test scale; a very large number of distinct
  tenants over a long-running gateway process would need a cleanup policy.
