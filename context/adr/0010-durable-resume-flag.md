# ADR-0010: durable_resume flag fixes false gap-detection on reconnect

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
ADR-0009's fleet simulator found: a device reconnecting without WAL-backed
resume state always looked to the gateway like it had lost events, even
when it hadn't. Confirmed at both small scale (50 devices, sandbox) and
real fleet scale (500 devices, 14-core hardware): 100% of sent events were
acked (zero real loss), yet the gateway logged a false `lost=N` on
essentially every reconnect.

## Decision
Added `bool durable_resume` to `Hello`. True (the real agent always sends
this) means `last_acked_seq` reflects genuine, WAL-backed resume state --
a seq mismatch on the first detection after `Hello` is a real gap. False
(the fleet simulator, which has no WAL by design) means the device is
telling the gateway up front that its resume state isn't meaningful -- the
gateway resyncs to whatever seq the first post-Hello detection carries,
with no gap counted, then resumes normal gap-checking for every detection
after that within the same stream.

## Why both directions had to be tested
A fix that simply stopped checking gaps on `durable_resume=false` would
"pass" by suppressing the false positive -- but so would a fix that broke
gap detection entirely, and the two are indistinguishable without a test
that also proves REAL gaps are still caught. Built
`ridgeline_gap_protocol_test`, a minimal raw gRPC client that sends an
exact, hand-picked seq jump, specifically so this could be tested directly:

- `durable_resume=false`, `last_acked_seq=0`, first seq=25 -> `lost=0` (fixed)
- `durable_resume=true`, `last_acked_seq=0`, first seq=25 (identical jump) -> `lost=24` (still caught)

`scripts/gap_detection_test.sh` runs both and fails if either regresses.

## Verification
- Reran the exact 50-device/forced-disconnect scenario that originally
  produced `lost=24` on every reconnect: now 0 false gaps across 100
  reconnects, `sent == acked` unchanged (100%).
- The 500-device real-hardware run showed the same pattern before this fix
  (100% acked, false `lost=` on nearly every reconnect) -- worth rerunning
  on real hardware after this fix to confirm `lost=0` there too.
- Both directions of `gap_detection_test.sh` pass.
- Full suite (39/39 net-only config) unaffected.

## Consequences
- Any future device implementation (Phase 3+ mTLS-authenticated devices,
  etc.) must set `durable_resume` honestly. A device that lies (claims
  `true` without real resume state) reintroduces exactly the bug this
  fixes, in the other direction -- worth a note in whatever device
  onboarding/attestation process Phase 3 designs, not just a protocol field
  nobody checks.
- This is a real precedent for Phase 3's device-identity work: a device's
  self-reported claims about its own state need either trust (as here) or
  verification (mTLS-backed attestation) depending on how much you trust
  the device population -- a decision Phase 3 should make explicitly.
