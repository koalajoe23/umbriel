#include "pacing/commit_timing_policy.h"

namespace umbriel {

  Nanoseconds refreshPeriod(Nanoseconds presentRefreshNsec, std::int32_t outputRefreshMhz) {
    if (presentRefreshNsec > 0) {
      return presentRefreshNsec;
    }
    if (outputRefreshMhz > 0) {
      return 1'000'000'000'000LL / outputRefreshMhz;
    }
    return kFallbackRefreshNsec;
  }

  Nanoseconds predictNextPresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now) {
    if (lastPresent == 0) {
      return now + period;
    }
    // Smallest lastPresent + k * period (k >= 1) strictly greater than now.
    const Nanoseconds elapsed = now - lastPresent;
    const Nanoseconds k = elapsed / period + 1;
    return lastPresent + k * period;
  }

  Nanoseconds predictFollowingPresent(Nanoseconds lastPresent, Nanoseconds period, Nanoseconds now) {
    return predictNextPresent(lastPresent, period, now) + period;
  }

  bool timedCommitDueStrict(Nanoseconds target, Nanoseconds framePresent) { return target <= framePresent; }

  Nanoseconds commitTimingEarlyTolerance(Nanoseconds period) { return period / kCommitTimingEarlyToleranceDivisor; }

  bool timedCommitDue(Nanoseconds target, Nanoseconds framePresent, Nanoseconds period) {
    return timedCommitDueStrict(target - commitTimingEarlyTolerance(period), framePresent);
  }

  Nanoseconds hiddenTickInterval(Nanoseconds lastKnownPeriod) {
    return lastKnownPeriod > 0 ? lastKnownPeriod : kHiddenFallbackRefreshNsec;
  }

  Nanoseconds frameWakeup(Nanoseconds target, Nanoseconds period) { return target - 2 * period; }

} // namespace umbriel
