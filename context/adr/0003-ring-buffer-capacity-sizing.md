# ADR-0003: Ring buffer capacity must scale with consumer throughput, not be a fixed default

- **Status:** Accepted
- **Date:** 2026-09-11

## Context

`ridgeline_bench` was run against `SpscRingBuffer<Event, 4096>` under two
conditions: an idle consumer (pops as fast as possible), and a 5ms/item
consumer delay, simulating a ~200fps-limited workload like real CPU
inference.

## What we found

Idle consumer: 5.6M items/sec throughput, p50 0.22us, p95 235us, p99 504us.
This is the ring buffer's own overhead and is the number recorded in
`BENCHMARKS.md`.

5ms-delay consumer: p99 enqueue-to-dequeue latency of ~22 **seconds**. Not a
bug — arithmetic. The buffer holds up to 4,095 usable slots. A consumer
that can only drain 200 items/sec falls behind immediately, the buffer
saturates near capacity, and every new item queues behind ~4,000 items
ahead of it: `4000 slots x 5ms/item ~= 20s`.

## Decision

Do not report the 22s figure as a general "latency" number — out of
context it reads as a broken system, when it's actually correctly-behaving
backpressure exposing an oversized buffer for that consumer speed.

Instead: **ring buffer capacity is chosen as (target max acceptable
staleness) x (consumer throughput)**, not left at a large default "to be
safe." A large capacity doesn't prevent backpressure, it just delays when
you notice it — and lets stale frames pile up past the point where a
detection on them is even useful.

For a wildfire-detection pipeline, a smoke event that's 20 seconds stale by
the time it reaches the alert engine has lost most of its value. Once
Phase 1b's real ONNX inference gives an actual per-frame cost, capacity
should be recomputed from that number, not guessed.

## Consequences

- Default capacity in `bench/ring_buffer_bench.cc` (4096) is appropriate
  ONLY for benchmarking the buffer's own overhead with a fast/idle
  consumer, not as a production default.
- Before Phase 1b ships, capacity must be chosen based on measured
  inference latency and a stated maximum tolerable staleness (e.g. "we
  accept up to 1s of latency at our target fps").
- Worth a metric in production: track buffer occupancy (`SizeApprox()`)
  over time. Consistently near-full is a signal the consumer can't keep
  up, independent of any single latency measurement.
