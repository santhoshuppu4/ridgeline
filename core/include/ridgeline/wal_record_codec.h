#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include "ridgeline/crc32.h"

namespace ridgeline {

// Isolated from Wal (file I/O) on purpose: this is the piece that parses
// untrusted-ish bytes (a possibly-truncated or bit-flipped tail from a crash
// mid-write), so it's the piece worth fuzzing directly, with no filesystem
// involved. tools/fuzz/wal_record_fuzzer.cc calls DecodeRecord on raw
// libFuzzer-provided bytes.

inline constexpr std::size_t kWalRecordHeaderBytes = 16;  // 8 (seq) + 4 (length) + 4 (crc32)
inline constexpr std::uint32_t kWalMaxPayloadBytes = 16u * 1024 * 1024;  // Sanity cap; real events are tiny.

inline std::string EncodeRecord(std::uint64_t seq, const std::string& payload) {
  std::string out;
  out.resize(kWalRecordHeaderBytes + payload.size());
  const auto len = static_cast<std::uint32_t>(payload.size());
  const std::uint32_t crc = Crc32(payload.data(), payload.size());
  std::memcpy(out.data(), &seq, 8);
  std::memcpy(out.data() + 8, &len, 4);
  std::memcpy(out.data() + 12, &crc, 4);
  std::memcpy(out.data() + kWalRecordHeaderBytes, payload.data(), payload.size());
  return out;
}

// Attempts to decode one record starting at buf[offset..]. On success,
// returns true, fills `seq` and `payload`, and advances `offset` past the
// record. On any failure (not enough bytes for a header, declared length
// longer than the sanity cap, not enough bytes for the payload, or a CRC
// mismatch), returns false and leaves `offset` UNCHANGED -- the caller
// (Wal::ReplayUnacked) treats "false" uniformly as "nothing more to
// replay," whether the cause is a clean end-of-file or a torn/corrupt tail
// record. That's a deliberate design choice: distinguishing "clean EOF"
// from "corruption" would require guessing intent from ambiguous bytes,
// which isn't reliable, and the correct action (stop replaying, don't
// crash) is identical either way.
//
// seq is stored in the record framing itself, NOT parsed out of the
// payload, so Wal can filter/compact by seq without knowing anything about
// what the payload actually is (a serialized protobuf DetectionEvent in
// this project, but Wal itself stays payload-format-agnostic).
inline bool DecodeRecord(const std::string& buf, std::size_t& offset, std::uint64_t& seq, std::string& payload) {
  if (offset + kWalRecordHeaderBytes > buf.size()) return false;

  std::uint32_t len = 0, stored_crc = 0;
  std::uint64_t record_seq = 0;
  std::memcpy(&record_seq, buf.data() + offset, 8);
  std::memcpy(&len, buf.data() + offset + 8, 4);
  std::memcpy(&stored_crc, buf.data() + offset + 12, 4);

  if (len > kWalMaxPayloadBytes) return false;  // Implausible length: almost certainly a corrupt/torn header.
  const std::size_t record_end = offset + kWalRecordHeaderBytes + len;
  if (record_end > buf.size() || record_end < offset) return false;  // record_end<offset guards size_t overflow.

  const char* payload_start = buf.data() + offset + kWalRecordHeaderBytes;
  if (Crc32(payload_start, len) != stored_crc) return false;

  seq = record_seq;
  payload.assign(payload_start, len);
  offset = record_end;
  return true;
}

}  // namespace ridgeline
