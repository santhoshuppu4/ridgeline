# ADR-0004: FakeDetector must model bursts, not isolated periodic hits

- **Status:** Accepted
- **Date:** 2026-09-11

## Context
`pipeline_test.cc` wires capture -> ring buffer -> detector -> K-of-N
confirmation together end to end. It needs a deterministic detector so the
expected output is computable by hand, not just "roughly right."

## What went wrong first
The first version of `FakeDetector` fired one isolated positive frame every
10 frames (`frame_index % 10 == 0`). Combined with K=3-of-N=5 confirmation,
this can NEVER confirm: with positives spaced 10 frames apart, no 5-frame
sliding window can contain more than one of them, so the positive count
inside any window never exceeds 1, and K=3 is unreachable. The test ran,
built cleanly, and failed with zero confirmed detections over 5000 frames
-- neither `FakeDetector` nor `KOfNConfirmer` was buggy in isolation
(each has its own passing unit tests), but the two configurations were
mathematically incompatible together. Only the integration test caught it.

## Decision
`FakeDetector` now takes `(burst_length, period)` and fires positive for
`burst_length` CONSECUTIVE frames out of every `period` frames -- modeling
a real detection event (smoke visible across a run of frames), not an
isolated blip. This is also more realistic: a real fire doesn't appear in
exactly one frame and vanish.

## Consequences
- Any test wiring a detector to a K-of-N confirmer must ensure the
  detector's positive-run length is compatible with N -- specifically,
  after enough consecutive positives to exceed the burst, a window of N
  consecutive frames must be able to contain >= K positives. As a rule of
  thumb: `burst_length >= K` is necessary (though not sufficient on its
  own -- work through the specific K, N, burst_length, period combination
  by hand, the way pipeline_test.cc's comment does, before trusting a
  "no detections confirmed" result to mean the pipeline is broken rather
  than the test parameters being incompatible).
- Once Phase 1b-ii swaps in a real ONNX detector, this same test still
  works as a smoke test for the wiring by keeping FakeDetector for that
  one test and adding separate tests specifically for the real detector's
  accuracy against labeled data.
