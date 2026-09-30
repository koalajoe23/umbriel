#pragma once

#include <cstdint>

namespace umbriel {

  using Nanoseconds = std::int64_t;

  // Used when neither the output's mode nor a compositor-reported present refresh interval is known.
  inline constexpr Nanoseconds kFallbackRefreshNsec = 16'666'667; // 60 Hz
  // Tick rate for a hidden output's commit-timing clock, which has no presentation feedback of its own to derive a
  // period from.
  inline constexpr Nanoseconds kHiddenFallbackRefreshNsec = 25'000'000; // 40 Hz
  // How many periods a recorded present stays a trustworthy phase for prediction (see `predictReleaseFramePresent`).
  inline constexpr Nanoseconds kPredictionFreshPeriods = 2;
  // How many periods a working pacer may go without a refresh before its stall watchdog emits one (see
  // `stallRefreshDelay`).
  inline constexpr Nanoseconds kStallRefreshPeriods = 4;
  // Tolerance layer over the strict rule (see `timedCommitDue`): period / kCommitTimingEarlyToleranceDivisor.
  inline constexpr Nanoseconds kCommitTimingEarlyToleranceDivisor = 4;

  // The output's present interval: a reported present-refresh wins, else the output mode's rate, else the 60 Hz
  // fallback.
  [[nodiscard]] Nanoseconds refreshPeriod(Nanoseconds presentRefreshNsec, std::int32_t outputRefreshMhz);

  // The next present time strictly after `now`: `now + period` if nothing has ever presented, otherwise the
  // smallest `lastPresent + k * period` (k >= 1) strictly greater than `now`.
  [[nodiscard]] Nanoseconds predictNextPresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now);

  // The present after the one `predictNextPresent` would give.
  [[nodiscard]] Nanoseconds predictFollowingPresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now);

  // The present a commit released at a refresh at `now` is expected to reach, while the last present is at most
  // kPredictionFreshPeriods periods old: `predictFollowingPresent` when the frame behind the refresh committed and its
  // flip is still pending (`presentPending`), since the released commit then renders at the next frame event and
  // presents a period after it; `predictNextPresent` when that frame committed nothing, since the released commit's
  // damage then gets a frame straight away, which flips at the very next vblank. Otherwise `now` itself: an output
  // that idled (headless frames in particular) resumes at an arbitrary phase, so a stale present extrapolates a
  // present up to a period too late; `now` is a lower bound, so a stale refresh releases only commits whose target has
  // passed, never early.
  [[nodiscard]] Nanoseconds
  predictReleaseFramePresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now, bool presentPending);

  // The commit-timing rule with no tolerance: a wp_commit_timer target is due once its present has arrived.
  [[nodiscard]] bool timedCommitDueStrict(Nanoseconds target, Nanoseconds framePresent);

  // How far ahead of its target a commit may release, so it is not held back by a frame that lands microseconds
  // early. Deleting this and its caller below drops the tolerance layer entirely.
  [[nodiscard]] Nanoseconds commitTimingEarlyTolerance(Nanoseconds period);

  // `timedCommitDueStrict` with `target` relaxed by `commitTimingEarlyTolerance(period)`. Removing the tolerance
  // layer makes this call `timedCommitDueStrict(target, framePresent)` directly.
  [[nodiscard]] bool timedCommitDue(Nanoseconds target, Nanoseconds framePresent, Nanoseconds period);

  // How often a hidden output's commit-timing clock should tick, absent presentation feedback of its own.
  [[nodiscard]] Nanoseconds hiddenTickInterval(Nanoseconds lastKnownPeriod);

  // The hidden tick after the one due at `previousDeadline`: one `interval` later, so ticks keep to a fixed grid and
  // millisecond timer rounding and wakeup latency never accumulate. If that is already not in the future (the loop
  // stalled), the grid restarts one interval from `now` rather than firing the missed ticks back to back.
  [[nodiscard]] Nanoseconds nextHiddenTick(Nanoseconds previousDeadline, Nanoseconds interval, Nanoseconds now);

  // When to wake up and start rendering a frame aimed at `target`: two periods early, to cover both composition and
  // the client's own render time.
  [[nodiscard]] Nanoseconds frameWakeup(Nanoseconds target, Nanoseconds period);

  // How long a working, visible pacer goes without a refresh before it emits one itself: kStallRefreshPeriods periods.
  // fifo-v1 lets the compositor clear a barrier early to ensure client forward progress, which covers an output that
  // stops producing frames (a session that lost its DRM device renders nothing until it is back).
  [[nodiscard]] Nanoseconds stallRefreshInterval(Nanoseconds period);

  // How long from `now` until the stall watchdog is due, given the last refresh at `lastRefresh`; 0 once overdue.
  [[nodiscard]] Nanoseconds stallRefreshDelay(Nanoseconds lastRefresh, Nanoseconds period, Nanoseconds now);

} // namespace umbriel
