#include "pacing/fifo_queue.h"

namespace umbriel {

  bool FifoQueue::shouldHold(bool waitBarrier, bool synchronizedSubsurface) const {
    if (!waitBarrier || synchronizedSubsurface) {
      return false;
    }
    // Even once the barrier itself clears, a commit still queued ahead of this one must be released first to keep
    // submission order, and a setter that has not applied yet has not had its refresh.
    return m_barrier || !m_queue.empty() || !m_inFlightSetters.empty();
  }

  void FifoQueue::hold(HeldCommit commit) { m_queue.push_back(Entry{.commit = commit, .order = m_nextOrder}); }

  void FifoQueue::committed(bool setsBarrier) {
    if (setsBarrier) {
      m_inFlightSetters.push_back(m_nextOrder);
    }
    ++m_nextOrder;
  }

  void FifoQueue::applied(bool setBarrier) {
    if (!setBarrier) {
      return;
    }
    m_barrier = true;
    if (!m_inFlightSetters.empty()) {
      m_inFlightSetters.pop_front();
    }
  }

  std::vector<std::uint32_t> FifoQueue::refresh() {
    std::vector<std::uint32_t> released;
    while (!m_queue.empty()) {
      const Entry entry = m_queue.front();
      // An unapplied setter ahead of this commit: releasing it could apply both in one flush.
      if (!m_inFlightSetters.empty() && m_inFlightSetters.front() < entry.order) {
        break;
      }
      m_queue.pop_front();
      released.push_back(entry.commit.seq);
      if (entry.commit.setsBarrier) {
        break;
      }
    }
    m_barrier = false;
    return released;
  }

  std::vector<std::uint32_t> FifoQueue::drain() {
    std::vector<std::uint32_t> released;
    released.reserve(m_queue.size());
    for (const Entry& entry : m_queue) {
      released.push_back(entry.commit.seq);
    }
    m_queue.clear();
    m_inFlightSetters.clear();
    m_barrier = false;
    return released;
  }

} // namespace umbriel
