#include "check.h"
#include "output/direct_scanout.h"

using umbriel::DirectScanoutState;
using umbriel::DirectScanoutTracker;
using Observation = umbriel::DirectScanoutTracker::Observation;

namespace {
  DirectScanoutState scannedOut(const char* windowId) {
    DirectScanoutState state;
    state.active = true;
    state.reason.clear();
    state.windowId = windowId;
    state.appId = "steam_app_223850";
    return state;
  }

  DirectScanoutState composited(const char* reason) {
    DirectScanoutState state;
    state.reason = reason;
    return state;
  }
} // namespace

UMBRIEL_TEST(startsInactiveWithoutACandidate) {
  const DirectScanoutTracker tracker;
  CHECK(!tracker.settled().active);
  CHECK_EQ(tracker.settled().reason, std::string("no_candidate"));
}

UMBRIEL_TEST(aNewStateStartsTheWaitOnce) {
  DirectScanoutTracker tracker;
  CHECK_EQ(tracker.observe(scannedOut("a")), Observation::Pending);
  // Every later frame in the same state leaves the running wait alone, so it can expire.
  CHECK_EQ(tracker.observe(scannedOut("a")), Observation::Unchanged);
  CHECK(!tracker.settled().active);
}

UMBRIEL_TEST(settlingAdoptsTheObservedState) {
  DirectScanoutTracker tracker;
  tracker.observe(scannedOut("a"));
  CHECK(tracker.settle());
  CHECK(tracker.settled().active);
  CHECK_EQ(tracker.settled().windowId, std::string("a"));
  CHECK(tracker.settledChangeIsNotable());
  // Nothing new to adopt.
  CHECK(!tracker.settle());
}

UMBRIEL_TEST(flickerBackToTheSettledStateDropsTheWait) {
  DirectScanoutTracker tracker;
  tracker.observe(scannedOut("a"));
  tracker.settle();
  // A cursor shown for one frame over the game.
  CHECK_EQ(tracker.observe(composited("software_cursor")), Observation::Pending);
  CHECK_EQ(tracker.observe(scannedOut("a")), Observation::Settled);
  CHECK(!tracker.settle());
  CHECK(tracker.settled().active);
}

UMBRIEL_TEST(switchingReasonsWhileCompositingIsNotNotable) {
  DirectScanoutTracker tracker;
  tracker.observe(composited("composited"));
  CHECK(tracker.settle());
  CHECK(!tracker.settledChangeIsNotable());
  tracker.observe(composited("effects"));
  CHECK(tracker.settle());
  CHECK(!tracker.settledChangeIsNotable());
  CHECK_EQ(tracker.settled().reason, std::string("effects"));
}

UMBRIEL_TEST(stoppingAndMovingWindowsAreNotable) {
  DirectScanoutTracker tracker;
  tracker.observe(scannedOut("a"));
  tracker.settle();
  tracker.observe(scannedOut("b"));
  CHECK(tracker.settle());
  CHECK(tracker.settledChangeIsNotable());
  tracker.observe(composited("composited"));
  CHECK(tracker.settle());
  CHECK(tracker.settledChangeIsNotable());
}

int main() { return RUN_TESTS(); }
