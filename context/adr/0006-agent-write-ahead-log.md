# ADR-0006: Agent durability via write-ahead log, and wiring the real pipeline into the agent

- **Status:** Accepted
- **Date:** 2026-09-11

## Context
Before this change, an agent crash between "detection confirmed" and "gateway
acked" silently lost events, and `ridgeline_edge`'s real detections never left
the process. Phase 1b-iii connects the real pipeline to the gRPC stream;
Phase 1c makes delivery survive agent crashes.

## Decisions

**Durability contract: at-least-once, across `kill -9`.** Every DetectionEvent
is appended to a WAL and fsync'd *before* it enters the send path. An event is
"committed" once `Wal::Append` returns. The gateway deduplicates redeliveries
by sequence number (ADR-0001). Not claimed: power-loss atomicity on hardware
with volatile disk write caches.

**Record format:** `[seq u64][len u32][crc32 u32][payload]`, little-endian.
The seq lives in the framing (not inside the protobuf payload) so the WAL can
filter and compact without knowing the payload format. Replay stops cleanly at
the first short, oversized, or CRC-failing record, which is exactly what a
crash mid-append leaves behind.

**Checkpoint via write-temp + fsync + rename.** The last acked seq is stored in
a separate file replaced atomically, so a crash mid-checkpoint leaves the old
value, never a torn one. A missing/malformed checkpoint means "nothing acked":
more redelivery, never data loss.

**Single file + Compact() instead of segment files (deviation from the original
plan).** Compaction rewrites only unacked records (temp + fsync + rename) when
the WAL has grown by 1MB. Segment rotation would avoid rewriting, but at edge
event rates the simpler design is adequate. Revisit if WAL rewrite time shows
up in latency measurements.

**Outbox uses a mutex, not a lock-free queue.** It is touched at event rate
(a few per second), not frame rate, and needs operations a ring buffer doesn't
offer (pop-by-ack, full snapshot for resend). Lock-free structures stay on the
frame hot path, where they earn their complexity.

**EdgePipeline extracted from ridgeline_edge.** The tool and the agent now run
the same capture/inference/K-of-N code, feeding a second SPSC ring buffer of
ConfirmedEvents. Heartbeats carry `queue_depth` and `frames_dropped`;
DetectionEvents carry `frames_confirmed` and `window_size`.

## Bugs found while building this (each caught by a test, not by review)

1. **Resend progress tracked by outbox size instead of seq.** Acks pop the front
   of the outbox while the producer appends to the back, so size-based indexing
   skipped or reordered events. Smoke test showed `lost=79`. Fixed by tracking
   the highest seq sent.
2. **CMake ordering.** `RIDGELINE_MODEL_PATH` was defined after the agent
   subdirectory consumed it, compiling an empty default model path. No
   configure/build error; crashed at runtime. Dependencies are now included first.
3. **Video mode never exited** without `--duration-s`: the exit condition
   required `time_up()`, which is always false when no duration is set. Caught
   by the video smoke test hanging until its timeout.
4. **The first chaos test could not detect data loss.** It checked only the
   gateway's gap counter and still passed with WAL appends disabled, because a
   WAL-less restart reuses sequence numbers for new events and the gateway sees
   a contiguous sequence. Rewritten with an identity oracle: agent logs
   `(seq, capture_ns)` after each fsync'd append, gateway logs what it receives,
   every commit must be received, and no seq may map to two different events.
   The scenario stops the gateway before killing the agent so events exist
   *only* in the WAL. Mutation-tested: disabling WAL appends and skipping replay
   each fail with exactly the 46 WAL-only events missing.

## Verification
- WAL unit tests: roundtrip, restart, stale acks, corrupted tail, truncated
  tail, compaction (+ restart after compaction). Disabling the CRC check fails 2.
- libFuzzer on `DecodeRecord` (ASan+UBSan): ~24M inputs in 60s, no findings.
- `scripts/smoke_test.sh`: gateway outage, 0 lost / 0 duplicates.
- `scripts/smoke_test_video.sh`: real video -> ONNX -> K-of-N -> gRPC, exactly 4 events.
- `scripts/chaos_test.sh`: kill -9 with 46 WAL-only events, all delivered.
- Full suite under Release, ASan+UBSan, and TSan (core).

## Known limitations
- `Wal` reopens the file per append; fine at event rate, not at frame rate.
- `FileSizeBytes()` reads the whole file; acceptable at MB scale, should use `stat()` before the WAL grows larger.
- Events produced while the smoke test waits for the gateway show multi-second
  `max_transit`: that's queueing time in the outbox during the outage, not network latency.
