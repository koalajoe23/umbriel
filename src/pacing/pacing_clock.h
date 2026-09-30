#pragma once

#include "pacing/commit_timing_policy.h"

#include <algorithm>
#include <climits>
#include <ctime>

namespace umbriel {

  inline constexpr Nanoseconds kNsecPerSec = 1'000'000'000;
  inline constexpr Nanoseconds kNsecPerMsec = 1'000'000;

  [[nodiscard]] inline Nanoseconds toNanoseconds(const timespec& time) {
    return (static_cast<Nanoseconds>(time.tv_sec) * kNsecPerSec) + time.tv_nsec;
  }

  // The presentation clock's current time.
  [[nodiscard]] inline Nanoseconds monotonicNow() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return toNanoseconds(now);
  }

  // wl_event_source timers count whole milliseconds and a delay of 0 disarms them, so both conversions below give at
  // least 1 and saturate at INT_MAX.

  // For a periodic tick, where a sub-millisecond error either way is harmless: rounds to the nearest millisecond.
  [[nodiscard]] inline int timerDelayMsecNearest(Nanoseconds delay) {
    const Nanoseconds msec = (std::max<Nanoseconds>(delay, 0) + (kNsecPerMsec / 2)) / kNsecPerMsec;
    return static_cast<int>(std::clamp<Nanoseconds>(msec, 1, INT_MAX));
  }

  // For a "not before" deadline: rounds up, so the timer never fires before the instant it was armed for, at most a
  // millisecond late.
  [[nodiscard]] inline int timerDelayMsecNotBefore(Nanoseconds delay) {
    const Nanoseconds msec = (std::max<Nanoseconds>(delay, 0) + kNsecPerMsec - 1) / kNsecPerMsec;
    return static_cast<int>(std::clamp<Nanoseconds>(msec, 1, INT_MAX));
  }

} // namespace umbriel
