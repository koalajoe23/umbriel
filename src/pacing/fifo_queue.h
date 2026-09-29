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
  class FifoQueue {
  public:
    // A commit waits when it requested `wait_barrier` and a barrier is currently set. Synchronized subsurfaces never
    // wait: their parent's commit already carries them.
    [[nodiscard]] bool shouldHold(bool waitBarrier, bool synchronizedSubsurface) const;
    void hold(HeldCommit commit);
    // Records whether the commit that just applied set the barrier, arming it for the next refresh.
    void applied(bool setBarrier);
    // Releases queued seqs up to and including the first that set the barrier, and clears the barrier. Releases
    // everything if none in the queue set it.
    [[nodiscard]] std::vector<std::uint32_t> refresh();
    // Releases every held seq and clears the barrier, for surface teardown.
    [[nodiscard]] std::vector<std::uint32_t> drain();
    [[nodiscard]] bool barrier() const { return m_barrier; }
    [[nodiscard]] bool empty() const { return m_queue.empty(); }

  private:
    std::deque<HeldCommit> m_queue;
    bool m_barrier = false;
  };

} // namespace umbriel
