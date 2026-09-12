# ADR-0009: Fleet device simulator

- **Status:** Accepted
- **Date:** 2026-09-12

## Context
ADR-0001 flagged the gateway's synchronous, one-thread-per-connection gRPC
server as something to "revisit before the Phase-5 fleet simulator; measure
thread count and p99 under load." Phase 2 builds that simulator and takes
the measurement.

## Decisions

**No per-device WAL.** The simulator's job is transport and scale --
connection handling, backoff/reconnect, aggregate throughput -- not
durability, which `scripts/chaos_test.sh` already proves thoroughly with a
real identity oracle. Reusing `ridgeline::BackoffPolicy`/`FullJitterBackoff`
(the same reconnect logic the real agent uses) instead of a second
implementation.

**Deterministic correctness check in CI, real scale numbers on real
hardware.** `scripts/fleet_smoke_test.sh` runs 10 devices with zero fault
injection and asserts every sent event is acked -- exact, not approximate.
It does NOT report throughput/latency: CI runners have few, often
throttled cores, and this project's own sandbox has exactly one, which
makes any timing number measured there meaningless (the same lesson from
the ring-buffer benchmark in ADR-0003). Real fleet numbers need
`ridgeline_device_simulator` run directly with a larger `--devices` count
on real multi-core hardware.

## Findings from running it

**Confirmed: ~1 thread per connection.** 150 simulated devices connected
produced 161 gateway threads (measured via `/proc/<pid>/status`), consistent
with the synchronous server design. Answers ADR-0001's open question with a
number rather than an assumption -- whether that number is a problem
depends on target fleet size, which real hardware testing at higher device
counts should establish before committing to an async-server rewrite.

**A real gap in reconnect-vs-gap-tracking, found by running 50 devices with
forced disconnects every 6s.** Every event was acked (100%, no real data
loss), but the gateway logged `lost=24` on every single reconnect. Cause:
the gateway's per-stream gap counter assumes a reconnecting device's
`Hello.last_acked_seq` reflects real resume state (as the WAL-backed real
agent's does). The simulator has no WAL, so it reconnects with
`last_acked_seq=0` while its own sequence numbers keep climbing -- the
gateway has no way to distinguish "a device restarted without persistent
state" from "a device lost 24 real events," and currently assumes the
latter. Real events were not lost (aggregate acked/sent confirms it); the
gateway's own diagnostic counter is what's misleading in this specific
scenario.

This is a real product gap, not just a simulator quirk: any device that
loses its WAL (disk failure, factory reset, a bug) and reconnects would
trigger the same false "lost" reading. Fixing it would need either a
separate per-device high-water-mark the gateway tracks independently of
`last_acked_seq` (so it can tell "seq jumped because the device is genuinely
ahead of what it told us" apart from "seq jumped because messages actually
went missing"), or an explicit session/epoch identifier a device includes on
reconnect. Documented here as a finding, not fixed in this phase -- worth
scoping as its own piece of work rather than folding into the simulator's
first delivery.

## Consequences
- `ridgeline_device_simulator`'s `--disconnect-every-s` is a genuinely
  useful fault-injection knob for future testing (config reconciliation,
  Phase 4's OTA rollback), not just this phase's thread-count measurement.
- The false-gap finding should inform Phase 3/4 design (device identity,
  reconnect protocol) rather than being patched reactively later.
