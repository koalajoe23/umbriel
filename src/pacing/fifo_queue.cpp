#include "pacing/fifo_queue.h"

namespace umbriel {

  bool FifoQueue::shouldHold(bool waitBarrier, bool synchronizedSubsurface) const {
    if (!waitBarrier || synchronizedSubsurface) {
      return false;
    }
    // Even once the barrier itself clears, a commit still queued ahead of this one must be released first to keep
    // submission order.
    return m_barrier || !m_queue.empty();
  }

  void FifoQueue::hold(HeldCommit commit) { m_queue.push_back(commit); }

  void FifoQueue::applied(bool setBarrier) { m_barrier = m_barrier || setBarrier; }

  std::vector<std::uint32_t> FifoQueue::refresh() {
    std::vector<std::uint32_t> released;
    while (!m_queue.empty()) {
      const HeldCommit commit = m_queue.front();
      m_queue.pop_front();
      released.push_back(commit.seq);
      if (commit.setsBarrier) {
        break;
      }
    }
    m_barrier = false;
    return released;
  }

  std::vector<std::uint32_t> FifoQueue::drain() {
    std::vector<std::uint32_t> released;
    released.reserve(m_queue.size());
    for (const HeldCommit& commit : m_queue) {
      released.push_back(commit.seq);
    }
    m_queue.clear();
    m_barrier = false;
    return released;
  }

} // namespace umbriel
