#include "pacing/pacing.h"

#include "pacing/commit_timing_manager.h"
#include "pacing/fifo_manager.h"

#include <wayland-server-core.h>

namespace umbriel {

  struct Pacing::Impl {
    wl_display* display = nullptr;
    std::unique_ptr<FifoManager> fifoManager;
    std::unique_ptr<CommitTimingManager> commitTimingManager;
  };

  Pacing::Pacing(wl_display* display) : m_impl(std::make_unique<Impl>()) {
    m_impl->display = display;
    m_impl->fifoManager = std::make_unique<FifoManager>(display);
    m_impl->commitTimingManager = std::make_unique<CommitTimingManager>(display);
  }

  Pacing::~Pacing() = default;

  bool Pacing::valid() const {
    return m_impl->display != nullptr && m_impl->fifoManager->valid() && m_impl->commitTimingManager->valid();
  }

} // namespace umbriel
