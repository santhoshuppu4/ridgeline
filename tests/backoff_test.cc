#include "ridgeline/backoff.h"
#include <gtest/gtest.h>
#include <limits>

using std::chrono::milliseconds;
using ridgeline::BackoffCeiling;
using ridgeline::BackoffPolicy;
using ridgeline::FullJitterBackoff;

namespace { const BackoffPolicy kPolicy{milliseconds{100}, milliseconds{1000}}; }

TEST(BackoffCeiling, DoublesEachAttemptUntilCap) {
  EXPECT_EQ(BackoffCeiling(0, kPolicy), milliseconds{100});
  EXPECT_EQ(BackoffCeiling(1, kPolicy), milliseconds{200});
  EXPECT_EQ(BackoffCeiling(3, kPolicy), milliseconds{800});
  EXPECT_EQ(BackoffCeiling(4, kPolicy), milliseconds{1000});
}
TEST(BackoffCeiling, HugeAttemptSaturatesAtCapWithoutOverflow) {
  EXPECT_EQ(BackoffCeiling(std::numeric_limits<std::uint32_t>::max(), kPolicy), kPolicy.cap);
}
TEST(FullJitterBackoff, StaysWithinZeroAndCeiling) {
  for (std::uint32_t attempt = 0; attempt < 20; ++attempt) {
    for (double u : {0.0, 0.25, 0.5, 0.999999}) {
      const auto d = FullJitterBackoff(attempt, kPolicy, u);
      EXPECT_GE(d, milliseconds{0});
      EXPECT_LE(d, BackoffCeiling(attempt, kPolicy));
    }
  }
}
