#pragma once

#include <cstdint>
#include <deque>
#include <vector>

namespace umbriel {

  // One commit held behind a fifo_v1 barrier: its release sequence number and whether it was the commit that
  // requested the barrier (`set_barrier`), which marks where a `refresh()` may stop releasing.
  struct HeldCommit {
    std::uint32_t seq;
    bool setsBarrier;
  };

  // Decision core for the fifo_v1 protocol: whether a commit must wait, and which held commits a display refresh or
  // a drain releases. Holds no wlroots state; a manager on top applies the hold and unlocks released seqs.
  //
  // It also tracks in-flight barrier setters: commits that carry `set_barrier` and have been committed but not yet
  // applied, whether this queue holds them, another lock does (commit-timing, a syncobj wait), or a refresh has just
  // released them. Their barrier is not active yet, so a wait_barrier commit behind one must not apply in the same
  // flush, which would leave the setter's content current for zero refresh cycles.
  class FifoQueue {
  public:
    // A commit waits when it requested `wait_barrier` and either a barrier is currently set, an in-flight setter is
    // ahead of it, or older commits are already queued (clearing the barrier does not let a new commit cut ahead of
    // them). Synchronized subsurfaces never wait: their parent's commit already carries them.
    [[nodiscard]] bool shouldHold(bool waitBarrier, bool synchronizedSubsurface) const;
    // Queues a commit shouldHold said must wait. Call before committed() for the same commit.
    void hold(HeldCommit commit);
    // Records every client commit, held or not, in commit order; one that sets the barrier is in flight until it
    // applies.
    void committed(bool setsBarrier);
    // Records whether the commit that just applied set the barrier, arming it for the next refresh and retiring the
    // oldest in-flight setter.
    void applied(bool setBarrier);
    // Clears the barrier and releases queued seqs from the head, up to and including the first that set the barrier,
    // stopping before any commit that an unapplied setter is ahead of. Releases everything if nothing blocks.
    [[nodiscard]] std::vector<std::uint32_t> refresh();
    // Releases every held seq and forgets the barrier and in-flight setters, for surface teardown.
    [[nodiscard]] std::vector<std::uint32_t> drain();
    [[nodiscard]] bool barrier() const { return m_barrier; }
    [[nodiscard]] bool empty() const { return m_queue.empty(); }
    // Nothing a refresh would change: no barrier, no held commit, no setter in flight.
    [[nodiscard]] bool idle() const { return !m_barrier && m_queue.empty() && m_inFlightSetters.empty(); }

  private:
    // A held commit and its position in commit order.
    struct Entry {
      HeldCommit commit;
      std::uint64_t order;
    };

    std::deque<Entry> m_queue;
    // Commit-order positions of the in-flight setters, oldest first. wlroots applies commits in order, so the oldest
    // is the next to apply.
    std::deque<std::uint64_t> m_inFlightSetters;
    // The position committed() gives the next client commit.
    std::uint64_t m_nextOrder = 0;
    bool m_barrier = false;
  };

} // namespace umbriel
