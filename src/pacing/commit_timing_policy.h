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

  // The present a commit released at a refresh at `now` is expected to reach: `predictFollowingPresent` while the last
  // present is at most kPredictionFreshPeriods periods old, else `now` itself. An output that idled (headless frames
  // in particular) resumes at an arbitrary phase, so a stale present extrapolates a present up to a period too late;
  // `now` is a lower bound, so a stale refresh releases only commits whose target has passed, never early.
  [[nodiscard]] Nanoseconds predictReleaseFramePresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now);

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

  // When to wake up and start rendering a frame aimed at `target`: two periods early, to cover both composition and
  // the client's own render time.
  [[nodiscard]] Nanoseconds frameWakeup(Nanoseconds target, Nanoseconds period);

} // namespace umbriel
