#include "ridgeline/kofn_confirmer.h"

#include <gtest/gtest.h>

using ridgeline::KOfNConfirmer;

TEST(KOfNConfirmer, RequiresKPositivesBeforeConfirming) {
  KOfNConfirmer c(/*k=*/3, /*n=*/5);
  EXPECT_FALSE(c.Update(true));   // 1/1 positive, but only 1 seen so far — not yet 3.
  EXPECT_FALSE(c.Update(true));   // 2/2
  EXPECT_TRUE(c.Update(true));    // 3/3 — hits K.
}

TEST(KOfNConfirmer, ToleratesMissesWithinWindow) {
  // K=2, N=4: 2 of the last 4 frames positive is enough, even with misses mixed in.
  KOfNConfirmer c(2, 4);
  EXPECT_FALSE(c.Update(true));   // [T]              1 positive
  EXPECT_FALSE(c.Update(false));  // [T,F]            1 positive
  EXPECT_TRUE(c.Update(true));    // [T,F,T]          2 positive -> confirmed
  EXPECT_TRUE(c.Update(false));   // [T,F,T,F]        still 2 positive, window full -> confirmed
}

TEST(KOfNConfirmer, OldPositivesAgeOutOfTheWindow) {
  // This is the case a plain increment/decrement counter without a real
  // window gets wrong: confirmation must be able to LAPSE once the
  // positive frames that earned it fall outside the last N.
  KOfNConfirmer c(2, 3);
  EXPECT_FALSE(c.Update(true));   // [T]
  EXPECT_TRUE(c.Update(true));    // [T,T] -> confirmed
  EXPECT_TRUE(c.Update(false));   // [T,T,F] -> still confirmed (2 of last 3)
  EXPECT_FALSE(c.Update(false));  // [T,F,F] -- oldest T aged out -> only 1 of last 3, no longer confirmed
}

TEST(KOfNConfirmer, AllPositivesStaysConfirmed) {
  KOfNConfirmer c(3, 5);
  for (int i = 0; i < 20; ++i) {
    EXPECT_EQ(c.Update(true), i >= 2) << "should confirm from the 3rd positive frame onward, iteration " << i;
  }
}

TEST(KOfNConfirmer, AllNegativesNeverConfirms) {
  KOfNConfirmer c(1, 5);
  for (int i = 0; i < 20; ++i) {
    EXPECT_FALSE(c.Update(false));
  }
}

TEST(KOfNConfirmer, ResetClearsWindowAndCount) {
  KOfNConfirmer c(1, 3);
  EXPECT_TRUE(c.Update(true));
  c.Reset();
  EXPECT_EQ(c.WindowSize(), 0u);
  EXPECT_EQ(c.PositiveCount(), 0u);
  EXPECT_FALSE(c.Update(false));  // Fresh state: one negative frame after reset, correctly not confirmed.
}

TEST(KOfNConfirmer, KEqualsNRequiresUnanimity) {
  KOfNConfirmer c(3, 3);
  EXPECT_FALSE(c.Update(true));
  EXPECT_FALSE(c.Update(true));
  EXPECT_FALSE(c.Update(false));  // 2 of 3 — one miss is enough to deny confirmation when K == N.
  EXPECT_FALSE(c.Update(true));
  EXPECT_FALSE(c.Update(true));
  EXPECT_TRUE(c.Update(true));    // Three T's in a row, window full of positives.
}

#if defined(__EXCEPTIONS) || !defined(NDEBUG)
// KOfNConfirmer(0, N) and K > N are documented as programmer errors
// (std::abort()), not runtime-recoverable conditions — analogous to an
// out-of-bounds vector access. GoogleTest's death test support runs the
// constructor in a forked child process, so aborting there doesn't take the
// whole test binary down with it.
TEST(KOfNConfirmerDeathTest, RejectsInvalidThresholds) {
  EXPECT_DEATH({ ridgeline::KOfNConfirmer c(0, 5); }, "");
  EXPECT_DEATH({ ridgeline::KOfNConfirmer c(6, 5); }, "");
  EXPECT_DEATH({ ridgeline::KOfNConfirmer c(1, 0); }, "");
}
#endif
