#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ridgeline {

// ---------------------------------------------------------------------------
// STUDY NOTES — read this before the code.
//
// WHAT PROBLEM THIS SOLVES
// The capture thread produces frames as fast as the camera delivers them.
// The inference thread consumes them as fast as the model runs. They run on
// different threads and must not block each other: if capture ever blocks
// waiting for inference to catch up, you lose the live edge of the video
// feed, which is the one thing you can't get back. A mutex-protected queue
// would let one thread block the other under contention, and it forces two
// threads through one lock even though there's only ever one producer and
// one consumer. That specialization is exactly what "SPSC" (single-producer,
// single-consumer) buys you: with only one writer and one reader, you can
// synchronize with two atomic indices and no lock at all.
//
// WHY IT'S CORRECT WITHOUT A LOCK
// The producer only ever writes `tail_`. The consumer only ever writes
// `head_`. Each side only *reads* the other's index. Because there's a
// single writer per index, there's no write-write race on either index —
// the only thing left to get right is making sure that when the consumer
// reads a slot after seeing an updated `tail_`, it actually sees the data
// the producer just wrote there, and not stale memory. That's what the
// acquire/release memory ordering below buys you (explained inline at each
// use). Get the ordering wrong and this becomes a data race that TSan will
// catch — which is exactly why tests run it under ThreadSanitizer.
//
// WHY CACHE-LINE PADDING
// `head_` is written only by the consumer; `tail_` is written only by the
// producer. If they land in the same 64-byte cache line, every write to one
// invalidates the other core's cached copy of the *whole line*, even though
// the two threads never touch each other's variable. That's "false sharing":
// two threads that don't logically conflict still fight over a cache line.
// Padding each atomic out to its own cache line stops that fight. This is
// exactly the kind of thing a naive queue gets wrong and a benchmark exposes.
//
// WHY THIS IS "ARENA ALLOCATION"
// The slot array below is allocated exactly once, in the constructor. Pushing
// a frame copies (or move-assigns) it into a pre-existing slot; it never
// calls `new`. That's the whole idea of an arena in a hot loop: pay the
// allocation cost once, up front, and never call malloc/free again on the
// per-frame path. Combined with a fixed-size Frame struct (see
// agent/frame.h, built alongside this), the capture loop never touches the
// heap.
//
// WHY "TryPush RETURNS bool" INSTEAD OF "DROP-OLDEST"
// The original design note said "drop-oldest: capture never blocks." True
// drop-oldest — overwrite the oldest *unconsumed* slot to make room — is
// unsafe here: the consumer might be mid-read of that exact slot when the
// producer overwrites it, which is a data race, not a queue. Fixing that
// safely needs per-slot sequence numbers (a "disruptor"-style ring) so the
// consumer can detect it's reading a stale/overwritten slot and bail out.
// That's real complexity worth adding *deliberately*, not by accident.
// This version instead rejects the push when full and lets the caller
// (agent/pipeline.h) increment a `frames_dropped` counter and move on —
// capture still never blocks, the drop still happens, but no slot is ever
// touched by both threads at once. Document this tradeoff yourself in your
// own ADR-0002 once you've rebuilt this — it's a legitimate design decision,
// and explaining why you chose the simpler-but-safe version over the
// harder-but-lossless one is exactly the kind of thing an interviewer wants
// to hear.
// ---------------------------------------------------------------------------

// GCC/Clang warn (-Winterference-size) that this constant's value depends on
// the target's -mtune flags and could silently change between builds if the
// ABI compatibility isn't pinned. We're using it only inside this header to
// size padding, not across a shared-library ABI boundary, so that risk
// doesn't apply here — hence the explicit suppression rather than papering
// over it with -Wno-interference-size globally.
#if defined(__cpp_lib_hardware_interference_size)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winterference-size"
#endif
inline constexpr std::size_t kCacheLineSize = std::hardware_destructive_interference_size;
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#else
// libstdc++ historically didn't define hardware_destructive_interference_size
// even in C++20 mode; 64 bytes is correct for essentially every x86_64 and
// ARM64 desktop/server chip you'll benchmark this on.
inline constexpr std::size_t kCacheLineSize = 64;
#endif

// Fixed-capacity, lock-free, single-producer/single-consumer ring buffer.
//
// CONSTRUCT THIS ON THE HEAP, NOT AS A STACK LOCAL, whenever T is large or
// Capacity is large. All `Capacity` slots of T live directly inside this
// object (that's the point — it's the arena), so sizeof(SpscRingBuffer<T,N>)
// is roughly N * sizeof(T). With T = Frame (~1.3MB, see frame.h) and even a
// small N, that object is several megabytes: bigger than a lot of default
// thread stacks (commonly ~8MB, and less in some environments), especially
// once other locals and sanitizer redzones are added on top. Prefer
// `auto rb = std::make_unique<SpscRingBuffer<Frame, N>>();` — one heap
// allocation, made once at startup, which is exactly the "allocate once,
// never again on the hot path" contract this class promises. (A test in
// tests/ring_buffer_test.cc originally crashed with an ASan stack-overflow
// for exactly this reason — worth reproducing yourself once, so the failure
// mode is one you recognize instantly in a real agent process.)
//
// T must be trivially safe to overwrite via move/copy-assignment — no
// invariant that spans multiple instances. `Capacity` MUST be a power of two:
// that turns the wraparound modulo into a bitwise AND, which is why the
// static_assert below exists rather than silently rounding up for you.
//
// Thread-safety contract: at most one thread may call TryPush (the
// "producer"), and at most one thread may call TryPop (the "consumer").
// Calling either from more than one thread concurrently is undefined
// behavior — this class deliberately does not defend against that, because
// the whole performance win comes from not paying for a general-purpose
// multi-producer/multi-consumer queue.
template <typename T, std::size_t Capacity>
class SpscRingBuffer {
  static_assert(Capacity >= 2, "Capacity must be at least 2");
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
  static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                "T must be movable or copyable without throwing; a throw mid-slot-write would "
                "leave the ring buffer's invariants broken with no way to roll back");

 public:
  SpscRingBuffer() = default;

  // Non-copyable, non-movable: the padding and atomics make relocating this
  // safely more trouble than it's worth, and nothing in this project needs
  // to move a live ring buffer around.
  SpscRingBuffer(const SpscRingBuffer&) = delete;
  SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

  // Producer side. Returns false (and does NOT touch `item`) if the buffer
  // is full — the caller decides what "full" means for them (drop it, count
  // it, log it). Never blocks, never allocates.
  bool TryPush(T item) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write tail_
    const std::size_t next_tail = (tail + 1) & kMask;

    // Acquire here pairs with the consumer's release store to head_ in
    // TryPop. Without it, we could observe a stale `head_` and conclude the
    // buffer is full when the consumer has actually already freed the slot —
    // a correctness bug, not just a missed optimization: the reverse case
    // (thinking there's room when there isn't) is the one that's actually
    // dangerous, and this ordering is what rules it out.
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (next_tail == head) {
      return false;  // Full: the next write would land on the slot the consumer hasn't read yet.
    }

    slots_[tail] = std::move(item);

    // Release here pairs with the consumer's acquire load of tail_ below.
    // It guarantees the write to slots_[tail] above is visible to the
    // consumer *before* the consumer can observe the new tail_ value and act
    // on it. Reorder this as a relaxed store and TSan (or a very unlucky
    // production run) will eventually catch the consumer reading a half
    // written slot.
    tail_.store(next_tail, std::memory_order_release);
    return true;
  }

  // Consumer side. Returns false if the buffer is empty. On success, `out`
  // receives the item via move-assignment and the slot is freed for reuse.
  bool TryPop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head_

    // Acquire here pairs with the producer's release store to tail_ in
    // TryPush, and is what makes this whole thing safe: it guarantees that
    // if we observe the producer's new tail_, we also observe every write
    // the producer made to the slot before publishing that tail_.
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;  // Empty.
    }

    out = std::move(slots_[head]);
    const std::size_t next_head = (head + 1) & kMask;

    // Release here pairs with the producer's acquire load of head_ above:
    // it publishes "this slot is free again" only after the move out of it
    // has completed.
    head_.store(next_head, std::memory_order_release);
    return true;
  }

  // ---------------------------------------------------------------------------
  // ZERO-COPY API (added in Phase 1b-ii).
  //
  // WHY THIS EXISTS: TryPush(T item) takes its argument BY VALUE and then
  // move-assigns it into the slot. For an int that's free. For a Frame
  // holding a 1280x720 BGR image inline (~2.6MB of std::array), "move" is a
  // full copy — so every frame paid two 2.6MB memcpys (into the parameter,
  // then into the slot), plus 2.6MB of stack for the parameter itself. That
  // defeats the whole "arena" idea: the slot memory already exists, so the
  // producer should write the pixels straight into it.
  //
  // TryPushWith(fill): calls fill(T& slot) on the next free slot IN PLACE,
  // and only publishes it (advances tail_) if fill returns true. If fill
  // returns false (e.g. the video decoder hit end-of-file), nothing is
  // published and the slot is simply reused next time.
  //
  // TryPopWith(consume): calls consume(T& slot) on the oldest slot IN PLACE,
  // then frees it (advances head_) after consume returns.
  //
  // WHY THIS IS STILL SAFE WITHOUT A LOCK — same argument as TryPush/TryPop,
  // just with the write/read happening inside a callback:
  //   - The producer only touches slots_[tail], and only BEFORE publishing
  //     the new tail_. Until tail_ advances, the consumer considers that slot
  //     empty and will never read it.
  //   - The consumer only touches slots_[head], and only AFTER seeing a
  //     tail_ that includes it and BEFORE publishing the new head_. Until
  //     head_ advances, the producer considers that slot full and will never
  //     write it (the "next_tail == head" full check).
  // So each slot has exactly one owner at any moment, and ownership is handed
  // across by a single atomic store. TSan-verified in
  // tests/ring_buffer_test.cc (RingBufferZeroCopy.* stress test).
  //
  // Contract: callbacks must not throw (the project builds without relying
  // on exceptions on the hot path) and must not call back into this ring
  // buffer. Keep them short — the consumer's callback runs while that slot is
  // still "occupied," so a slow consume() makes the buffer look fuller to the
  // producer, which is exactly the backpressure ADR-0003 describes.
  // ---------------------------------------------------------------------------
  template <typename Fill>
  bool TryPushWith(Fill&& fill) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t next_tail = (tail + 1) & kMask;
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (next_tail == head) {
      return false;  // Full.
    }
    if (!fill(slots_[tail])) {
      return false;  // Producer declined to publish; slot stays unpublished.
    }
    tail_.store(next_tail, std::memory_order_release);
    return true;
  }

  template <typename Consume>
  bool TryPopWith(Consume&& consume) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
      return false;  // Empty.
    }
    consume(slots_[head]);
    head_.store((head + 1) & kMask, std::memory_order_release);
    return true;
  }

  // Snapshot only — by the time the caller reads the result, the real value
  // may have already changed, since the other thread runs concurrently.
  // Useful for metrics/logging (e.g. heartbeat queue depth), not for control
  // flow: never write `if (!Empty()) TryPop(...)`, just call TryPop and
  // check its return value.
  std::size_t SizeApprox() const {
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    const std::size_t head = head_.load(std::memory_order_acquire);
    return (tail - head) & kMask;
  }

  static constexpr std::size_t capacity() { return Capacity - 1; }  // one slot always kept empty (see below)

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  // Why capacity() is Capacity - 1, not Capacity:
  // head == tail must unambiguously mean "empty" so TryPop can check it with
  // one comparison. If a full buffer were also allowed to reach head == tail,
  // full and empty would be indistinguishable. Reserving one slot — never
  // letting tail advance onto head — keeps "full" and "empty" distinct
  // without a separate counter (which would itself need to be atomic and
  // shared, defeating the point). This is the standard, well-known tradeoff
  // for this exact design; a size counter is the alternative, at the cost of
  // a second cross-thread atomic on the hot path.

  // The arena: allocated once, here, as part of the object itself — no heap
  // allocation, ever, on the push/pop path.
  T slots_[Capacity]{};

  // Padding: each atomic gets its own cache line so producer and consumer
  // writes never invalidate a line the other thread is actively reading.
  alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
  alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};

  // Trailing pad so this object doesn't share its last cache line with
  // whatever the allocator places right after it on the heap.
  char pad_[kCacheLineSize > sizeof(std::atomic<std::size_t>)
                ? kCacheLineSize - sizeof(std::atomic<std::size_t>)
                : kCacheLineSize]{};
};

}  // namespace ridgeline
