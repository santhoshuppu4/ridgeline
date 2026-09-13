#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES.
//
// WHAT THIS IS FOR: per-tenant rate limiting on the gateway's detection-
// event path, so one misbehaving or misconfigured device (or a compromised
// one) can't consume the gateway's capacity at another tenant's expense.
// This is the "rate-limit token buckets" line from the original design
// spec, actually built.
//
// THE ALGORITHM: a bucket holds up to `capacity` tokens, refills
// continuously at `tokens_per_second`, and each unit of work consumes one
// token. If the bucket is empty, the request is rejected (not queued,
// not delayed) -- the caller decides what "rejected" means (the gateway
// simply doesn't ack, letting the agent's own WAL-backed resend retry
// later, exactly like a Kafka publish failure already does in this
// project). This is the standard token-bucket shape: it allows a burst up
// to `capacity` (unlike a strict fixed-window rate limit, which can be
// unfairly strict right at a window boundary), while still bounding the
// long-run average rate to `tokens_per_second`.
//
// WHY AN INJECTABLE CLOCK, NOT std::chrono::steady_clock INTERNALLY: a
// rate limiter's whole behavior is defined in terms of elapsed time, so a
// test that has to sleep in real wall-clock time to prove "the bucket
// refills after N seconds" is slow AND flaky (any scheduling jitter
// changes the outcome near a boundary). Taking `now_ns` as a parameter to
// every call means a test can advance time in exact, instantaneous,
// arbitrary jumps and assert exact token counts -- this is the same
// "inject rather than call directly" idea as HttpTransport in
// device_shadow_store.h, applied to time instead of network I/O.
//
// THREAD SAFETY: NOT thread-safe by itself, same as most of this project's
// small building blocks -- the gateway serializes access with its own
// per-tenant mutex (see gateway/src/main.cc), since the actual call
// volume (per detection event, not per frame) doesn't need a lock-free
// design to keep up.
// ---------------------------------------------------------------------------

class TokenBucket {
 public:
  TokenBucket(double capacity, double tokens_per_second, std::int64_t now_ns)
      : capacity_(capacity), tokens_per_second_(tokens_per_second), tokens_(capacity), last_refill_ns_(now_ns) {}

  // Attempts to consume `cost` tokens (1.0 for "one detection event") as of
  // `now_ns`. Refills first (based on elapsed time since the last call),
  // then either deducts `cost` and returns true, or leaves the bucket
  // untouched and returns false.
  bool TryConsume(double cost, std::int64_t now_ns) {
    Refill(now_ns);
    if (tokens_ < cost) return false;
    tokens_ -= cost;
    return true;
  }

  // Snapshot only, for metrics/logging -- refills as of `now_ns` first, so
  // the reported value reflects "right now," not whenever the bucket was
  // last touched by a TryConsume call.
  double TokensAvailable(std::int64_t now_ns) {
    Refill(now_ns);
    return tokens_;
  }

 private:
  void Refill(std::int64_t now_ns) {
    if (now_ns <= last_refill_ns_) return;  // Clock went backwards or didn't advance; nothing to add.
    const double elapsed_seconds = static_cast<double>(now_ns - last_refill_ns_) / 1e9;
    tokens_ = std::min(capacity_, tokens_ + elapsed_seconds * tokens_per_second_);
    last_refill_ns_ = now_ns;
  }

  double capacity_;
  double tokens_per_second_;
  double tokens_;
  std::int64_t last_refill_ns_;
};

}  // namespace ridgeline
