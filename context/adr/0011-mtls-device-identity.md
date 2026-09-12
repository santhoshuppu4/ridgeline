# ADR-0011: mTLS device identity

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
`device_id` was a self-reported string in `Hello`, with nothing stopping a
device from claiming to be a different one. ADR-0010's "Consequences"
section flagged this explicitly: a device's claims about itself need
either trust or cryptographic verification, and that decision should be
made deliberately.

## Decisions

**mTLS with per-device client certificates, Common Name == device_id.**
`scripts/generate_test_certs.sh` creates a dev CA and issues one cert per
device with its device_id as the CN. The gateway (`--tls-ca`, `--tls-cert`,
`--tls-key`) requires and verifies client certificates
(`GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY`); the agent
presents its own device cert with the same three flags. Without these
flags on both sides, everything is unchanged plaintext gRPC -- fully
backward compatible with every existing script.

**Identity cross-check happens on `Hello`, using the TLS layer's own
verified identity, not a new mechanism.** `context->auth_context()->
FindPropertyValues(GRPC_X509_CN_PROPERTY_NAME)` reads the cert CN gRPC's
handshake already cryptographically verified. If it doesn't match the
claimed `device_id`, the gateway rejects the connection
(`PERMISSION_DENIED`) rather than proceeding. On a plaintext connection
this list is empty and the check is a no-op -- one code path handles both
mTLS-on and mTLS-off.

## Testing: three scenarios, not one

A fix that only tests "impersonation gets rejected" could still be wrong
in two other ways: too strict (breaks legitimate devices) or not actually
enforced (a bypass path). `scripts/mtls_test.sh` verifies all three,
against a real gRPC TLS handshake, not a mock:

1. **Matching identity** (own cert, own claimed device_id) -> works normally.
2. **Impersonation** (device B's cert, claims to be device A) -> rejected
   on every retry; zero events ever delivered, confirmed by grepping for
   any "connected" line for the impersonated identity (there is none).
3. **No TLS at all** against an mTLS-required gateway -> the TLS handshake
   itself fails; the connection never reaches application code at all.

## A real, unrelated bug found while writing scenario 1's test

The fake-detection producer's rate loop capped every sleep at 50ms, but
incremented its "next due" time by the full period every iteration
regardless of whether that period actually elapsed. For any
`--rate-hz >= 20` (period <= 50ms) this is harmless -- the 50ms cap never
binds. For `--rate-hz < 20`, "next due" drifts further ahead of real time
every iteration, and the 50ms cap becomes the ACTUAL rate: a fixed ~20
events/sec regardless of the configured rate. Invisible for the entire
project until now because every prior test (smoke_test.sh at 50Hz,
chaos_test.sh at 30Hz, the fleet simulator's 1-2Hz defaults measured over
seconds, not fractions of a second) happened to use rate_hz >= 20 or not
notice a few extra events. `scripts/mtls_test.sh`'s `--rate-hz=5
--duration-s=2` was the first test to actually need a low rate over a
short, exact window, and got 40 events instead of 10 -- exactly `2s / 50ms`,
not `2s * 5Hz`. Fixed: the loop now waits the full period (in short slices,
so `g_stop`/duration_s cutoffs stay responsive) BEFORE emitting, rather
than emitting first and hoping the next sleep catches up. Verified: the
same command now produces exactly 10 events, and the full suite (all of
which used rate_hz >= 20) is unaffected.

## A known issue, documented rather than fixed here

Confirmed separately while investigating the rate bug: the agent's
`--duration-s` is a SOFT limit. Its exit condition requires the outbox to
be empty (`(time_up() || producer_finished()) && outbox.Size() == 0`), so
if the gateway is unreachable, undelivered events never clear the outbox
and the agent runs forever regardless of `--duration-s` -- confirmed
directly: a run against an intentionally-unreachable gateway exceeded a
300-second wait with no sign of stopping. This is arguably correct for a
real production agent (never abandon undelivered data on a soft timer),
but it means `--duration-s` cannot be relied on as a hard test timeout
whenever delivery might fail -- which is why every test script in this
project (`kafka_gateway_smoke_test.sh`'s unreachable-broker case,
`mtls_test.sh`'s scenario 3) wraps agent invocations in an external `timeout`
rather than trusting `--duration-s` alone. Not fixed here: a real fix needs
a deliberate design decision (a separate hard-cutoff flag? accept data loss
past a deadline? only in test/simulator builds?) rather than a quick patch
that might quietly weaken the durability guarantee ADR-0006 built.

## Consequences
- Real device provisioning (issuing a cert per physical device, rotation,
  revocation) is out of scope here -- `generate_test_certs.sh` is
  explicitly dev/test only, as its own header comment says.
- The CN-based identity check is a template for Phase 3's multi-tenancy:
  the same mechanism (verify a claimed identity against a cryptographic
  fact) extends naturally to verifying a claimed tenant_id once
  multi-tenancy exists.
