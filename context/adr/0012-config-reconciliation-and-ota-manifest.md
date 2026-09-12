# ADR-0012: Config reconciliation and signed OTA manifest

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
The `ConfigUpdate`/`GatewayMessage.config` proto fields have existed since
Phase 0 but were never used. Phase 4 wires them into a real desired/reported
reconciliation loop, and builds the cryptographic trust primitive
underneath signed OTA.

## Config reconciliation

**`Heartbeat` gained `applied_config_version`.** The gateway compares this
against a desired-config source (`--device-configs=path`, a simple
`device_id,version,confirm_k,confirm_n,confidence_threshold,target_fps`
line format -- deliberately not JSON, since this feature has nothing to do
with Kafka/Redis/DynamoDB and shouldn't need their dependencies) and pushes
a `ConfigUpdate` only when the device is behind. Confirmed convergence: the
gateway does not keep re-pushing once the agent reports the new version.

**Applying config means different things in the two agent modes, both
real, both verified end to end, not just logged:**
- Fake-detection mode: `target_fps` becomes the event rate directly.
  Verified: 5Hz -> 15Hz mid-run produced 91 events over 6s (a flat 5Hz
  would give ~30) -- the rate change demonstrably took effect, not just
  printed a log line.
- `--video` mode: `confirm_k`/`confirm_n`/`confidence_threshold` are fixed
  at `EdgePipeline` construction (see edge_pipeline.h), so applying a new
  value means stopping the current pipeline and rebuilding it with the new
  parameters -- a real, clean restart (RAII: `pipeline` going out of scope
  stops both its internal threads), not a placeholder. Verified against a
  real video file with a real config push.

## Signed OTA: manifest trust only, scoped deliberately

**What this is:** Ed25519 signing/verification of an OTA manifest
(version, binary SHA-256, URL, timestamp), via OpenSSL directly, with a
CLI tool (`ridgeline_ota_tool genkey|sign|verify`).

**What this is NOT, stated explicitly rather than silently omitted:**
downloading the binary, atomic A/B partition swapping, or
watchdog-triggered rollback. Those need real bootloader/OS-level
infrastructure a portfolio project can't meaningfully fake. Same reasoning
as the Kafka mock-vs-real split in ADR-0007: build the piece that can be
fully and honestly verified now, name the rest as explicit follow-up.

**Canonical, non-JSON serialization is what gets signed**
(`OtaManifest::CanonicalBytes()`), specifically so there is exactly one
definition of "what the signature covers" -- a signer and verifier that
serialized independently could disagree on field order or whitespace,
either breaking legitimate manifests or, worse, letting a reformatted
malicious manifest slip through.

**Tested with 4 independent mutation tests, not one.** Tampering with the
version, binary hash, URL, or timestamp are each tested separately and
each independently invalidates the signature -- proving the canonical
bytes actually cover every field, not just whichever one happened to get
tested. Also verified: a signature from a different keypair fails against
this public key; a truncated or empty signature fails cleanly rather than
crashing.

**The CLI tool was tested against real on-disk tampering**, not just
in-memory object mutation: `scripts/ota_test.sh` signs a real manifest,
verifies it, edits the actual file on disk with `sed`, and confirms
verification now fails -- closer to what a real attacker intercepting the
file could do than mutating a C++ struct in a unit test.

## Verification
- 13 unit tests (`ota_tests`), all mutation-style tests passing.
- `scripts/ota_test.sh`: sign, verify (pass), tamper on disk, verify
  (fail), wrong-signer, verify (fail) -- all real files, real keys.
- `scripts/config_reconciliation_test.sh`: real rate change end to end,
  convergence (exactly one push).
- Full existing suite (smoke/gap/mTLS/chaos scripts, 59 unit tests)
  unaffected.

## Consequences
- `--device-configs` reload-per-heartbeat (no caching) is fine at this
  scale; a real fleet-scale version would cache with an mtime check.
- Real OTA still needs: binary distribution infrastructure, an
  A/B-partitioned bootloader, and a watchdog -- none of which exist yet.
  `ridgeline_ota_tool` is the trust layer those would sit on top of, not a
  replacement for them.
