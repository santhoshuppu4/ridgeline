#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT PROBLEM THIS SOLVES: the agent can crash (power loss, OOM-kill, a bug)
// between "detected something" and "gateway durably has it." Without a WAL,
// anything in that window is gone forever — the frame that triggered it is
// long past by the time the process restarts. Writing every DetectionEvent to
// disk BEFORE sending it means a restart can replay exactly what wasn't
// acked, instead of silently losing it.
//
// FORMAT: an append-only file of records:
//   [4 bytes: payload length, little-endian] [4 bytes: CRC32 of payload] [payload bytes]
// fsync'd after every append. The length+CRC let replay detect a torn record
// (the file ends mid-write, e.g. from kill -9) and stop cleanly there instead
// of interpreting garbage as a corrupt-but-parseable record.
//
// DURABILITY BOUNDARY, NAMED EXPLICITLY: fsync makes a record durable against
// a process crash or `kill -9` (the OS page cache is flushed to the block
// device). It does NOT protect against the entire machine losing power
// mid-fsync on some filesystems/hardware (write barriers, disk write caches).
// For an edge device on a SD card behind a battery, process-crash durability
// is the realistic target — full power-loss atomicity would need
// hardware/filesystem guarantees this class doesn't attempt to provide, and
// says so rather than implying more than it delivers.
//
// SIMPLIFICATION VS. THE ORIGINAL SPEC (documented in ADR-0006, not hidden
// here): the spec described "segment files" so truncation is cheap. This
// implementation uses a SINGLE growing file plus a separate checkpoint file
// recording the last acked seq, and a Compact() method that rewrites the WAL
// to contain only unacked records. Compact() is O(unacked records), same as
// segment rotation would be, but the plain file keeps growing between
// Compact() calls, which segments avoid. For an edge agent's event rate this
// is a reasonable trade: much simpler code, and the caller controls when to
// pay the compaction cost (e.g. on a timer, not on every ack).
//
// THREAD SAFETY: NOT thread-safe. One Wal instance is owned by one thread
// (the agent's network/send thread in practice) — same single-owner
// discipline as everything else in this pipeline; concurrency crosses
// component boundaries via ring buffers, not via locks inside a component.
// ---------------------------------------------------------------------------

struct WalRecord {
  std::uint64_t seq = 0;
  std::string payload;  // Serialized DetectionEvent bytes (proto SerializeAsString()).
};

class Wal {
 public:
  // wal_path: the append-only log. checkpoint_path: small file holding the
  // last acknowledged seq. Both are created if missing. Throws
  // std::runtime_error if either can't be opened for read+write.
  Wal(std::string wal_path, std::string checkpoint_path);
  ~Wal();

  Wal(const Wal&) = delete;
  Wal& operator=(const Wal&) = delete;

  // Appends one record and fsyncs before returning. Throws std::runtime_error
  // on any I/O failure -- a WAL write that silently fails defeats the whole
  // point, so this fails loudly rather than returning a status the caller
  // might not check.
  void Append(std::uint64_t seq, const std::string& payload);

  // Durably records that the gateway has everything up to and including
  // up_to_seq (matches Ack.up_to_seq in the proto). Written via write-temp +
  // fsync + rename, so a crash mid-checkpoint-write leaves the OLD checkpoint
  // intact (rename is atomic on the same filesystem), never a torn one.
  void Acknowledge(std::uint64_t up_to_seq);

  std::uint64_t LastAcked() const { return last_acked_seq_; }

  // Calls on_record(seq, payload) for every record with seq > LastAcked(),
  // in ascending seq order. seq is read from the WAL record's own framing
  // (not parsed out of the payload -- Wal stays agnostic to what the
  // payload actually is). Stops (without error) at the first record that
  // is missing, truncated, or fails its CRC check -- that's the expected
  // shape of "the process died mid-append," and replay treats it as "nothing
  // more to replay," not as corruption to report. Returns the number of
  // records replayed.
  std::size_t ReplayUnacked(const std::function<void(std::uint64_t seq, const std::string& payload)>& on_record);

  // Rewrites the WAL file to contain only records with seq > LastAcked(),
  // via write-to-temp + fsync + atomic rename. Safe to call at any time,
  // including with zero unacked records (produces an empty WAL). A crash
  // during Compact() leaves the ORIGINAL wal file untouched, since the
  // rename only happens after the new file is fully written and synced.
  void Compact();

  // Bytes currently in the WAL file. Exposed so a caller can decide when
  // Compact() is worth doing (e.g. "compact when > 10MB").
  std::uint64_t FileSizeBytes() const;

 private:
  std::string wal_path_;
  std::string checkpoint_path_;
  std::uint64_t last_acked_seq_ = 0;

  void LoadCheckpoint();
};

}  // namespace ridgeline
