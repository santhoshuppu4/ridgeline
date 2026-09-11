// libFuzzer harness for DecodeRecord (core/include/ridgeline/wal_record_codec.h).
//
// This is exactly the function Wal::ReplayUnacked calls on file contents
// that could be a legitimately-truncated or corrupted tail from a crash --
// i.e. semi-trusted bytes. Fuzzing it directly (no file I/O, no Wal class)
// keeps each iteration to nanoseconds, so a short fuzzing run already
// explores millions of inputs.
//
// Build (needs clang, not gcc -- gcc has no -fsanitize=fuzzer):
//   clang++ -std=c++20 -Icore/include -fsanitize=fuzzer,address,undefined \
//     tools/fuzz/wal_record_fuzzer.cc -o wal_record_fuzzer
//
// Run:
//   ./wal_record_fuzzer -max_total_time=60
//
// A crash saves the failing input to a `crash-<hash>` file in the current
// directory -- feed it back in as a regression test via
// `./wal_record_fuzzer crash-<hash>` to reproduce deterministically.

#include <cstddef>
#include <cstdint>
#include <string>

#include "ridgeline/wal_record_codec.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string buf(reinterpret_cast<const char*>(data), size);
  std::size_t offset = 0;
  std::uint64_t seq = 0;
  std::string payload;
  // The only property under test: DecodeRecord must never crash, hang, or
  // read out of bounds on ANY input, including adversarial ones. It is
  // explicitly allowed to return false for garbage input -- that's correct
  // behavior, not a bug. Calling it in a loop until it stops advancing
  // additionally exercises Wal::ReplayUnacked's actual usage pattern
  // (repeated decode calls over one buffer), not just a single call.
  while (offset < buf.size()) {
    const std::size_t before = offset;
    if (!ridgeline::DecodeRecord(buf, offset, seq, payload)) break;
    if (offset <= before) break;  // Must always make forward progress on success.
  }
  return 0;
}
