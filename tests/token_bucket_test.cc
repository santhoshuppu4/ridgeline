#include "ridgeline/token_bucket.h"

#include <gtest/gtest.h>

using ridgeline::TokenBucket;

namespace {
constexpr std::int64_t kSecond = 1'000'000'000LL;
}

TEST(TokenBucket, StartsFullAtCapacity) {
  TokenBucket bucket(/*capacity=*/10, /*tokens_per_second=*/5, /*now_ns=*/0);
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(0), 10.0);
}

TEST(TokenBucket, ConsumesExactlyOneTokenPerCall) {
  TokenBucket bucket(10, 5, 0);
  EXPECT_TRUE(bucket.TryConsume(1.0, 0));
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(0), 9.0);
}

TEST(TokenBucket, RejectsWhenEmptyWithoutTouchingTheBucket) {
  TokenBucket bucket(/*capacity=*/2, /*tokens_per_second=*/1, /*now_ns=*/0);
  EXPECT_TRUE(bucket.TryConsume(1.0, 0));
  EXPECT_TRUE(bucket.TryConsume(1.0, 0));
  EXPECT_FALSE(bucket.TryConsume(1.0, 0)) << "bucket should be empty after consuming its full capacity";
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(0), 0.0) << "a rejected TryConsume must not deduct anything";
}

TEST(TokenBucket, RefillsExactlyProportionalToElapsedTime) {
  TokenBucket bucket(/*capacity=*/10, /*tokens_per_second=*/2, /*now_ns=*/0);
  ASSERT_TRUE(bucket.TryConsume(10.0, 0));  // Drain completely.
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(0), 0.0);

  // 1 second at 2 tokens/sec = exactly 2 tokens, checked at an exact
  // instantaneous jump -- no sleeping, no timing tolerance needed.
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(1 * kSecond), 2.0);
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(3 * kSecond), 6.0);
}

TEST(TokenBucket, RefillNeverExceedsCapacity) {
  TokenBucket bucket(/*capacity=*/5, /*tokens_per_second=*/100, /*now_ns=*/0);
  ASSERT_TRUE(bucket.TryConsume(1.0, 0));
  // At 100 tokens/sec, even a tiny elapsed time would overshoot capacity if
  // clamping were missing -- 1 full second is comically more than enough
  // to expose a missing std::min.
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(1 * kSecond), 5.0);
}

TEST(TokenBucket, SustainedRateAboveLimitIsThrottledButBelowLimitIsNot) {
  // Capacity 1 (no burst allowance), refill 2/sec -- i.e. one token every
  // 500ms. Requesting every 400ms (faster than refill) must eventually
  // fail; requesting every 600ms (slower than refill) must always succeed.
  TokenBucket fast_caller(1, 2, 0);
  int successes = 0, failures = 0;
  for (int i = 0; i < 20; ++i) {
    const std::int64_t now = i * 400'000'000LL;  // every 400ms
    if (fast_caller.TryConsume(1.0, now)) ++successes; else ++failures;
  }
  EXPECT_GT(failures, 0) << "a caller faster than the refill rate must eventually be throttled";
  EXPECT_GT(successes, 0) << "but not EVERY call should fail -- the bucket does allow its sustainable rate through";

  TokenBucket slow_caller(1, 2, 0);
  int all_succeeded = 0;
  for (int i = 0; i < 20; ++i) {
    const std::int64_t now = i * 600'000'000LL;  // every 600ms -- slower than the 500ms/token refill rate
    if (slow_caller.TryConsume(1.0, now)) ++all_succeeded;
  }
  EXPECT_EQ(all_succeeded, 20) << "a caller slower than the refill rate should never be throttled";
}

TEST(TokenBucket, ClockNotAdvancingIsSafeNoOp) {
  TokenBucket bucket(5, 10, 1000);
  ASSERT_TRUE(bucket.TryConsume(1.0, 1000));
  // Calling again with the SAME or an EARLIER timestamp (e.g. a caller with
  // slightly skewed clocks calling concurrently) must not crash or refill
  // based on a negative elapsed time.
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(1000), 4.0);
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(500), 4.0);
}

TEST(TokenBucket, FractionalCostIsSupported) {
  // Not used by the gateway today (1 detection = 1 token), but the API
  // supports non-integer costs, and it's worth confirming that's not
  // accidentally truncated to an integer somewhere.
  TokenBucket bucket(1.0, 1.0, 0);
  EXPECT_TRUE(bucket.TryConsume(0.5, 0));
  EXPECT_DOUBLE_EQ(bucket.TokensAvailable(0), 0.5);
  EXPECT_TRUE(bucket.TryConsume(0.5, 0));
  EXPECT_FALSE(bucket.TryConsume(0.1, 0));
}
