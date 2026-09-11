#pragma once

#include <cstdint>
#include <cstdlib>
#include <deque>

namespace ridgeline {

// STUDY NOTE: what problem this solves and why it's a sliding window, not a
// simple counter.
//
// A per-frame detector fires on noise: a bird crossing the frame, a cloud
// shadow, a sensor artifact. Confirming a detection only after it appears in
// at least K of the last N frames is what turns "the model said maybe" into
// "we're confident enough to escalate." This is a deliberately simple,
// well-understood technique (hysteresis / debouncing, the same family of
// idea as a Schmitt trigger) — not something that needs a learned model.
//
// Why a sliding window (deque) instead of a running counter:
// A naive "increment on hit, decrement on miss, confirm at threshold"
// counter can't correctly *un-confirm* when an old hit ages out of the
// window — it has no memory of *which* frames contributed. A window of the
// last N raw yes/no results lets Confirmed() be computed directly by
// counting true values in the window, so "was frame N+1 confirmed" is
// always answerable independent of history before the window.
class KOfNConfirmer {
 public:
  // K must be <= N. K == 0 would confirm on every frame including the first
  // (vacuously true), which defeats the point of confirmation, so it's
  // rejected rather than silently accepted.
  KOfNConfirmer(std::uint32_t k, std::uint32_t n) : k_(k), n_(n) {
    if (n_ == 0 || k_ == 0 || k_ > n_) {
      // A confirmer with a broken threshold is worse than no confirmer: it
      // would either never confirm (silently dropping every real
      // detection) or always confirm (defeating the point). Fail loudly at
      // construction instead of failing silently at every frame after.
      std::abort();
    }
  }

  // Feed one frame's raw (unconfirmed) detection result. Returns true if,
  // counting this frame, at least K of the last N frames were positive —
  // i.e. this frame is the one that pushes the count over the threshold, or
  // keeps it there.
  //
  // IMPORTANT: this returns whether the window is CURRENTLY confirmed, not
  // an edge-triggered "just became confirmed" signal. If the caller only
  // wants to act once per confirmed episode (e.g. emit one DetectionEvent,
  // not one per frame while confirmed), track the previous return value
  // and act only on the false->true transition. That's a caller-side
  // decision, deliberately not baked in here, since some callers (a live
  // dashboard showing "currently confirmed: yes/no") want the level, not
  // the edge.
  bool Update(bool raw_positive) {
    window_.push_back(raw_positive);
    if (raw_positive) ++positive_count_;

    if (window_.size() > n_) {
      if (window_.front()) --positive_count_;
      window_.pop_front();
    }

    return positive_count_ >= k_;
  }

  // Resets to the empty-window state. Use when a device reconnects after a
  // long gap (Phase 1c/WAL replay) — frames before the gap shouldn't count
  // toward confirming a detection after it, since they may be arbitrarily
  // stale relative to now.
  void Reset() {
    window_.clear();
    positive_count_ = 0;
  }

  std::uint32_t k() const { return k_; }
  std::uint32_t n() const { return n_; }
  std::size_t WindowSize() const { return window_.size(); }  // For DetectionEvent.window_size / frames_confirmed.
  std::uint32_t PositiveCount() const { return positive_count_; }

 private:
  std::uint32_t k_;
  std::uint32_t n_;
  std::deque<bool> window_;
  std::uint32_t positive_count_ = 0;
};

}  // namespace ridgeline
