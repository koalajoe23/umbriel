#include "check.h"
#include "pacing/commit_timing_policy.h"

using namespace umbriel;

// Strict group: exercises only `timedCommitDueStrict`. These must keep passing verbatim if the tolerance layer
// (`commitTimingEarlyTolerance` / `timedCommitDue`) is ever removed.

UMBRIEL_TEST(periodPrefersPresentRefresh) { CHECK_EQ(refreshPeriod(8'333'333, 60000), 8'333'333); }

UMBRIEL_TEST(periodFallsBackToOutputMode) { CHECK_EQ(refreshPeriod(0, 144000), 6'944'444); }

UMBRIEL_TEST(periodFallsBackTo60Hz) { CHECK_EQ(refreshPeriod(0, 0), kFallbackRefreshNsec); }

UMBRIEL_TEST(predictsFromNeverPresented) { CHECK_EQ(predictNextPresent(0, 16'666'667, 1'000'000'000), 1'016'666'667); }

UMBRIEL_TEST(predictsAcrossStaleGap) {
  CHECK_EQ(predictNextPresent(1'000'000'000, 10'000'000, 1'035'000'000), 1'040'000'000);
}

UMBRIEL_TEST(predictsStrictlyAfterNow) {
  CHECK_EQ(predictNextPresent(1'000'000'000, 10'000'000, 1'010'000'000), 1'020'000'000);
}

UMBRIEL_TEST(predictsWhenNowPrecedesLastPresent) {
  // A clock or present timestamp regression: `now` is behind `lastPresent` by more than one period. k >= 1 still
  // holds, so the answer is one period past `lastPresent`, never earlier than `lastPresent` itself.
  CHECK_EQ(predictNextPresent(1'000, 10, 800), 1'010);
}

UMBRIEL_TEST(followingIsOnePeriodLater) {
  CHECK_EQ(predictFollowingPresent(1'000'000'000, 10'000'000, 1'005'000'000), 1'020'000'000);
}

UMBRIEL_TEST(releaseFrameFreshPendingUsesFollowing) {
  // The frame committed: its flip is still pending, so a released commit renders at the next frame event and presents
  // one period after it.
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'005'000'000, true), 1'020'000'000);
}

UMBRIEL_TEST(releaseFrameFreshIdleUsesNext) {
  // The frame committed nothing: a released commit's damage gets a frame straight away, which flips at the next vblank.
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'005'000'000, false), 1'010'000'000);
}

UMBRIEL_TEST(releaseFrameFreshAtTwoPeriods) {
  // Exactly kPredictionFreshPeriods periods old still counts as fresh.
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'020'000'000, true), 1'040'000'000);
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'020'000'000, false), 1'030'000'000);
}

UMBRIEL_TEST(releaseFrameIdleAtFlipUsesNextVblank) {
  // DRM: the frame event of a flip that brought nothing new arrives with that flip's present, so the next present is
  // one period on.
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'000'000'000, false), 1'010'000'000);
}

UMBRIEL_TEST(releaseFrameStaleUsesNow) {
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'025'000'000, true), 1'025'000'000);
  CHECK_EQ(predictReleaseFramePresent(1'000'000'000, 10'000'000, 1'025'000'000, false), 1'025'000'000);
}

UMBRIEL_TEST(releaseFrameNeverPresentedUsesNow) {
  CHECK_EQ(predictReleaseFramePresent(0, 10'000'000, 5, true), 5);
  CHECK_EQ(predictReleaseFramePresent(0, 10'000'000, 5, false), 5);
}

UMBRIEL_TEST(strictReleasesAtBoundary) {
  CHECK(timedCommitDueStrict(500, 500));
  CHECK(!timedCommitDueStrict(501, 500));
}

UMBRIEL_TEST(hiddenTickUsesLastPeriod) {
  CHECK_EQ(hiddenTickInterval(6'944'444), 6'944'444);
  CHECK_EQ(hiddenTickInterval(0), kHiddenFallbackRefreshNsec);
}

UMBRIEL_TEST(wakeupTwoPeriodsEarly) { CHECK_EQ(frameWakeup(1'000'000'000, 10'000'000), 980'000'000); }

UMBRIEL_TEST(stallRefreshAfterFourPeriods) {
  CHECK_EQ(kStallRefreshPeriods, 4);
  CHECK_EQ(stallRefreshInterval(16'666'667), 66'666'668);
}

UMBRIEL_TEST(stallRefreshDelayCountsFromLastRefresh) {
  // Right after a refresh the watchdog waits the whole interval; later, only what is left of it.
  CHECK_EQ(stallRefreshDelay(1'000'000'000, 10'000'000, 1'000'000'000), 40'000'000);
  CHECK_EQ(stallRefreshDelay(1'000'000'000, 10'000'000, 1'015'000'000), 25'000'000);
}

UMBRIEL_TEST(stallRefreshDelayZeroOnceOverdue) {
  CHECK_EQ(stallRefreshDelay(1'000'000'000, 10'000'000, 1'040'000'000), 0);
  CHECK_EQ(stallRefreshDelay(1'000'000'000, 10'000'000, 2'000'000'000), 0);
}

// Tolerance group: deleted along with `commitTimingEarlyTolerance` and `timedCommitDue` if the tolerance layer is
// removed.

UMBRIEL_TEST(toleranceIsQuarterPeriod) { CHECK_EQ(commitTimingEarlyTolerance(16'000'000), 4'000'000); }

UMBRIEL_TEST(toleranceReleasesSlightlyEarly) {
  CHECK(timedCommitDue(1'004'000'000, 1'000'000'000, 16'000'000));
  CHECK(!timedCommitDue(1'004'000'001, 1'000'000'000, 16'000'000));
}

int main() { return RUN_TESTS(); }
