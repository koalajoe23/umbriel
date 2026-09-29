#include "pacing/fifo_queue.h"

#include "check.h"

using umbriel::FifoQueue;
using umbriel::HeldCommit;

UMBRIEL_TEST(holdsOnlyWhenBarrierSet) {
  FifoQueue queue;
  CHECK(!queue.shouldHold(true, false));
  queue.applied(true);
  CHECK(queue.shouldHold(true, false));
}

UMBRIEL_TEST(ignoresSynchronizedSubsurface) {
  FifoQueue queue;
  queue.applied(true);
  CHECK(!queue.shouldHold(true, true));
}

UMBRIEL_TEST(appliedWithoutBarrierKeepsBarrier) {
  // `applied` arms the barrier; it must not disarm it. A later commit that did not itself set the barrier is not
  // proof the barrier was released, so it must not clear a barrier still awaiting its refresh.
  FifoQueue queue;
  queue.applied(true);
  queue.applied(false);
  CHECK(queue.barrier());
  CHECK(queue.shouldHold(true, false));
}

UMBRIEL_TEST(ignoresCommitsWithoutWait) {
  FifoQueue queue;
  queue.applied(true);
  CHECK(!queue.shouldHold(false, false));
}

UMBRIEL_TEST(keepsOrderBehindQueue) {
  // refresh() clears the barrier, but the third commit is still queued (it set the barrier itself, so refresh stops
  // after releasing it): a new commit must still wait behind it to keep submission order.
  FifoQueue queue;
  queue.applied(true);
  queue.hold({1, false});
  queue.hold({2, true});
  queue.hold({3, true});
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{1, 2}));
  CHECK(!queue.barrier());
  CHECK(queue.shouldHold(true, false));
}

UMBRIEL_TEST(refreshReleasesThroughFirstBarrier) {
  FifoQueue queue;
  queue.hold({1, false});
  queue.hold({2, true});
  queue.hold({3, true});
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{1, 2}));
  CHECK(!queue.empty());
}

UMBRIEL_TEST(refreshWithoutBarrierReleasesAll) {
  FifoQueue queue;
  queue.hold({1, false});
  queue.hold({2, false});
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{1, 2}));
  CHECK(!queue.barrier());
}

UMBRIEL_TEST(drainReleasesEverything) {
  FifoQueue queue;
  queue.hold({1, false});
  queue.hold({2, true});
  queue.hold({3, true});
  CHECK_EQ(queue.drain(), (std::vector<std::uint32_t>{1, 2, 3}));
  CHECK(queue.empty());
  CHECK(!queue.barrier());
}

// In-flight setters: a commit that set the barrier but has not applied yet (another lock holds it, or a refresh just
// released it) keeps its barrier from counting as active, so a wait_barrier commit behind it must not apply in the same
// flush.

UMBRIEL_TEST(inFlightSetterHoldsNextWait) {
  FifoQueue queue;
  queue.committed(true);
  CHECK(!queue.barrier());
  CHECK(queue.shouldHold(true, false));
}

UMBRIEL_TEST(inFlightNonSetterDoesNotHold) {
  FifoQueue queue;
  queue.committed(false);
  CHECK(!queue.shouldHold(true, false));
}

UMBRIEL_TEST(appliedSetterLetsNextRefreshRelease) {
  FifoQueue queue;
  queue.committed(true);
  queue.hold({7, true});
  queue.committed(true);
  // The setter ahead has not applied: no refresh can count for its barrier yet.
  CHECK(queue.refresh().empty());
  queue.applied(true);
  CHECK(queue.barrier());
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{7}));
}

UMBRIEL_TEST(releasedSetterBlocksUntilApplied) {
  FifoQueue queue;
  queue.applied(true);
  queue.hold({1, true});
  queue.committed(true);
  queue.hold({2, false});
  queue.committed(false);
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{1}));
  // Released but still held by another lock: it has not applied, so commit 2 stays held and new waits queue behind.
  CHECK(queue.refresh().empty());
  CHECK(queue.shouldHold(true, false));
  queue.applied(true);
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{2}));
  CHECK(queue.empty());
}

UMBRIEL_TEST(laterSetterDoesNotBlockEarlierHeld) {
  // A setter committed after a held commit is behind it: releasing the held commit cannot apply it early.
  FifoQueue queue;
  queue.applied(true);
  queue.hold({1, false});
  queue.committed(false);
  queue.committed(true);
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{1}));
}

UMBRIEL_TEST(appliedSetterRetiresInFlight) {
  FifoQueue queue;
  queue.committed(true);
  queue.applied(true);
  CHECK_EQ(queue.refresh(), (std::vector<std::uint32_t>{}));
  CHECK(!queue.barrier());
  CHECK(!queue.shouldHold(true, false));
}

int main() { return RUN_TESTS(); }
