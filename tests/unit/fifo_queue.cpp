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

int main() { return RUN_TESTS(); }
