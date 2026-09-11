#include "ridgeline/ring_buffer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <numeric>
#include <thread>
#include <vector>

using ridgeline::SpscRingBuffer;

namespace {

// A plain int is enough for correctness tests; the Frame-based stress test
// below is what exercises the "arena" story with realistic-sized slots.
using IntRing = SpscRingBuffer<int, 8>;  // capacity() == 7 usable slots

}  // namespace

TEST(RingBufferBasics, StartsEmpty) {
  IntRing rb;
  int out = -1;
  EXPECT_FALSE(rb.TryPop(out));
  EXPECT_EQ(out, -1);  // TryPop must not touch `out` on failure.
  EXPECT_EQ(rb.SizeApprox(), 0u);
}

TEST(RingBufferBasics, PushThenPopSameOrder) {
  IntRing rb;
  ASSERT_TRUE(rb.TryPush(10));
  ASSERT_TRUE(rb.TryPush(20));
  ASSERT_TRUE(rb.TryPush(30));

  int out = 0;
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out, 10);
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out, 20);
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out, 30);
  EXPECT_FALSE(rb.TryPop(out));
}

TEST(RingBufferBasics, RejectsPushWhenFull) {
  IntRing rb;
  for (int i = 0; i < static_cast<int>(IntRing::capacity()); ++i) {
    ASSERT_TRUE(rb.TryPush(i)) << "push " << i << " should have succeeded (capacity=" << IntRing::capacity() << ")";
  }
  EXPECT_FALSE(rb.TryPush(999)) << "buffer should be full and reject further pushes";

  int out = -1;
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out, 0) << "popping should free a slot without reordering what's left";
  EXPECT_TRUE(rb.TryPush(999)) << "after freeing one slot, exactly one more push should succeed";
}

TEST(RingBufferBasics, WrapsAroundCorrectly) {
  // Push and pop repeatedly, well past `capacity`, so the internal head/tail
  // indices wrap multiple times. This is the case a naive (non-power-of-two,
  // or off-by-one) implementation gets wrong.
  //
  // NOTE: an earlier version of this test popped only 2 out of every 3
  // rounds, which grows the queue's depth by roughly one every three rounds
  // forever. Run out past ~21 rounds and the buffer is legitimately full
  // (capacity() == 7 here) and TryPush correctly starts returning false —
  // that's the ring buffer behaving correctly and the TEST being wrong, not
  // a bug in TryPush. Caught by exactly the kind of "does the queue ever
  // legitimately fill up mid-test" check you should run on your own tests
  // before trusting a green run. Fixed here to alternate push/pop so depth
  // stays bounded well under capacity while still wrapping the index
  // hundreds of times.
  IntRing rb;
  int next_expected_push = 0;
  int next_expected_pop = 0;

  for (int round = 0; round < 200; ++round) {
    ASSERT_TRUE(rb.TryPush(next_expected_push++));
    int out = -1;
    ASSERT_TRUE(rb.TryPop(out));
    EXPECT_EQ(out, next_expected_pop++);

    if (round % 5 == 0) {  // occasionally let depth build up to 3, then it drains below via the next iterations
      ASSERT_TRUE(rb.TryPush(next_expected_push++));
      ASSERT_TRUE(rb.TryPush(next_expected_push++));
      int a = -1, b = -1;
      ASSERT_TRUE(rb.TryPop(a));
      ASSERT_TRUE(rb.TryPop(b));
      EXPECT_EQ(a, next_expected_pop++);
      EXPECT_EQ(b, next_expected_pop++);
    }
  }
  EXPECT_EQ(next_expected_pop, next_expected_push);
  EXPECT_EQ(rb.SizeApprox(), 0u);
}

TEST(RingBufferBasics, SizeApproxTracksPushesAndPops) {
  IntRing rb;
  EXPECT_EQ(rb.SizeApprox(), 0u);
  ASSERT_TRUE(rb.TryPush(1));
  ASSERT_TRUE(rb.TryPush(2));
  EXPECT_EQ(rb.SizeApprox(), 2u);
  int out = 0;
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(rb.SizeApprox(), 1u);
}

TEST(RingBufferBasics, MoveOnlyTypeCompilesAndWorks) {
  // The push/pop API uses move-assignment, not copy, so this must work for
  // move-only payloads too (a real Frame with a unique_ptr member, etc.).
  struct MoveOnly {
    MoveOnly() = default;
    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&&) = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    int value = 0;
  };
  SpscRingBuffer<MoveOnly, 4> rb;
  ASSERT_TRUE(rb.TryPush(MoveOnly{42}));
  MoveOnly out;
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out.value, 42);
}

// ---------------------------------------------------------------------------
// Concurrent stress test: a real producer thread and a real consumer thread,
// hammering the buffer for several seconds. This is the test that actually
// validates the acquire/release reasoning in the header comments — run it
// under TSan (see scripts/, RIDGELINE_SANITIZE=thread) and under plain
// Release with a long duration to also shake out logic bugs that only show
// up under real interleavings, which TSan's own scheduling perturbation
// helps surface.
//
// Correctness check: every value the producer pushes is 0, 1, 2, 3, ... in
// order (a monotonic counter). The consumer verifies it receives that exact
// sequence with no gaps, no duplicates, and no reordering. That would catch
// a torn write, a lost wakeup, or a reordering bug that a "did it crash?"
// test would miss entirely.
// ---------------------------------------------------------------------------
TEST(RingBufferStress, ConcurrentProducerConsumerPreservesOrderAndCount) {
  constexpr std::uint64_t kTotal = 2'000'000;
  SpscRingBuffer<std::uint64_t, 1024> rb;
  std::atomic<bool> producer_done{false};
  std::atomic<std::uint64_t> pushed{0};
  std::atomic<std::uint64_t> dropped{0};

  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kTotal; ++i) {
      while (!rb.TryPush(i)) {
        ++dropped;         // Buffer momentarily full: spin and retry.
        std::this_thread::yield();  // Not a busy-loop in the pathological sense — just backs off for the consumer.
      }
      pushed.fetch_add(1, std::memory_order_relaxed);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    std::uint64_t expected = 0;
    std::uint64_t value = 0;
    while (expected < kTotal) {
      if (rb.TryPop(value)) {
        ASSERT_EQ(value, expected) << "ordering violated at index " << expected;
        ++expected;
      } else {
        std::this_thread::yield();
      }
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(pushed.load(), kTotal);
  // `dropped` isn't a correctness signal here (TryPush retries until it
  // succeeds), just useful to see printed when the test is slow.
  std::fprintf(stderr, "[stress] pushed=%llu retried_full=%llu\n", static_cast<unsigned long long>(kTotal),
               static_cast<unsigned long long>(dropped.load()));
}

// ---------------------------------------------------------------------------
// Arena story: push actual Frame objects through the ring buffer. This is
// the combination the edge agent will use — SpscRingBuffer<Frame, N> — and
// it's worth its own test because Frame is much larger than an int (roughly
// 1.3MB with the default 1280x720 NV12 sizing), which is exactly the case
// cache-line padding and in-place construction are meant for.
// ---------------------------------------------------------------------------
#include <memory>

#include "ridgeline/frame.h"

TEST(RingBufferWithFrame, PushAndPopRealFrame) {
  // Frame is ~1.3MB (see frame.h). A ring buffer of 4 Frame slots embeds
  // ~5.3MB of storage directly in the SpscRingBuffer object. Declaring THAT
  // as a plain stack local blows the default ~8MB thread stack once you add
  // a Frame-sized local on top of it and ASan's redzone overhead — this test
  // originally crashed with AddressSanitizer: stack-overflow for exactly
  // that reason. The fix, and the real-world lesson: "arena, allocated
  // once" still means one heap allocation for the whole arena, made once at
  // startup — not zero allocations ever. Only the per-push/per-pop path is
  // allocation-free. The agent will construct its ring buffer the same way:
  // heap-allocated once in main(), never as a stack local.
  auto rb_ptr = std::make_unique<ridgeline::SpscRingBuffer<ridgeline::Frame, 4>>();
  auto& rb = *rb_ptr;

  ridgeline::Frame f;
  std::vector<std::byte> fake_pixels(64, std::byte{0xAB});
  ASSERT_TRUE(f.CopyFrom(fake_pixels.data(), fake_pixels.size(), /*w=*/8, /*h=*/8,
                         /*capture_ns=*/123456789, /*index=*/1));
  ASSERT_TRUE(rb.TryPush(std::move(f)));

  ridgeline::Frame out;
  ASSERT_TRUE(rb.TryPop(out));
  EXPECT_EQ(out.width, 8u);
  EXPECT_EQ(out.height, 8u);
  EXPECT_EQ(out.used_bytes, 64u);
  EXPECT_EQ(out.frame_index, 1u);
  EXPECT_EQ(static_cast<unsigned char>(out.data[0]), 0xAB);
}

TEST(RingBufferWithFrame, CopyFromRejectsOversizedInputInsteadOfTruncating) {
  ridgeline::Frame f;
  std::vector<std::byte> too_big(ridgeline::Frame::kMaxBytes + 1);
  EXPECT_FALSE(f.CopyFrom(too_big.data(), too_big.size(), 9999, 9999, 0, 0));
  EXPECT_EQ(f.used_bytes, 0u) << "a rejected copy must not partially mutate the frame";
}
