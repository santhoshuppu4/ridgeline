# Study guide: rebuilding the SPSC ring buffer yourself

Goal: after this, close every file here and rewrite `ring_buffer.h` and
`frame.h` from memory, then diff against the reference. That diff is your
real study signal — every line that doesn't match is either something you
haven't internalized yet, or a legitimate design choice you made
differently (which is fine, as long as you can defend it).

## Order to read things in

1. `core/include/ridgeline/ring_buffer.h` — read the top comment block first,
   in full, before any code. It explains *why* each decision was made, not
   just what the code does.
2. `core/include/ridgeline/frame.h` — the payload type. Small, read it in one pass.
3. `tests/ring_buffer_test.cc` — read this like a spec. Each test name is a
   claim about behavior; the body is the proof.

## Three real bugs this reference implementation hit — reproduce them yourself

Don't skip this. Fixing a bug you triggered yourself is worth more than
reading about one.

**1. A logic bug in the test itself (not the ring buffer).**
`WrapsAroundCorrectly` originally popped only 2 out of every 3 rounds. That
grows the queue forever; eventually it legitimately fills and `TryPush`
correctly starts returning `false`. The test's `ASSERT_TRUE` then failed —
correctly reporting that *the test's assumption* was wrong, not that the
ring buffer was broken.
  - Reproduce: change the modulo condition back to `round % 3 != 0` and rerun.
  - Lesson: when a test fails, your first question is "which of my
    assumptions is wrong," not "what's broken in the code." Sometimes it's
    the test.

**2. Stack overflow from declaring a large ring buffer as a local variable.**
`SpscRingBuffer<Frame, 4>` embeds 4 Frames directly (~5.3MB) inside the
object. Declared as a plain stack local under ASan, it overflowed the
thread's default stack.
  - Reproduce: in a scratch test, write
    `ridgeline::SpscRingBuffer<ridgeline::Frame, 4> rb;` as a local instead of
    `std::make_unique<...>()`, build with `-DRIDGELINE_SANITIZE=address`, and run it.
  - Lesson: "arena, allocated once" still means one real heap allocation, made
    once. Zero allocations *ever* isn't the goal — zero allocations *per frame*
    is. Know the difference and be ready to explain it out loud.

**3. Weakening the memory ordering silently breaks correctness.**
Swap every `memory_order_acquire`/`memory_order_release` in `ring_buffer.h`
for `memory_order_relaxed` and TSan reports a data race on the very first
stress-test run — reliably, not intermittently.
  - Reproduce:
    ```
    sed -i 's/memory_order_release/memory_order_relaxed/g; s/memory_order_acquire/memory_order_relaxed/g' \
      core/include/ridgeline/ring_buffer.h
    cmake --build build-tsan
    ./build-tsan/tests/core_tests --gtest_filter=RingBufferStress.*
    ```
    Then revert (`git checkout -- core/include/ridgeline/ring_buffer.h`, or restore from your own copy).
  - Lesson: this is the single most interview-relevant fact about the whole
    file. Be able to say, specifically, which write the acquire pairs with
    and why a relaxed load could observe the updated index without observing
    the data that index is supposed to guard.

## Questions to answer out loud, from memory, before you consider this "known"

- Why does this need to be a *power of two* capacity? What operation would
  break if it weren't?
- Why is `capacity()` equal to `Capacity - 1`, not `Capacity`? What ambiguity
  does reserving one slot resolve?
- Walk through what happens, instruction by instruction, if you delete the
  `acquire` on the `head_` load inside `TryPush`. What's the actual failure
  mode — not "it's unsafe," but the specific sequence of events that
  produces wrong behavior?
- Why does this class refuse to be copied or moved? What would need to
  change to make that safe?
- This implementation rejects a push when full (`drop-newest`), while the
  original project spec said "drop-oldest." Explain the tradeoff in your own
  words, and what you'd need to add (per-slot sequence numbers) to safely
  implement true drop-oldest instead.
- Why does `SizeApprox()` exist as a separate, clearly-labeled "approximate"
  method instead of just calling it `size()`?

## What "done" looks like

You've rebuilt `ring_buffer.h` and `frame.h` without looking at the
reference, your version passes the same test file unmodified (or you've
written your own equivalent tests and can justify any behavioral
differences), and it's clean under both ASan+UBSan and TSan. At that point
the resume line about this component is something you built, not something
you watched get built.
