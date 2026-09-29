#include "pacing/pacing.h"

#include "pacing/fifo_manager.h"

#include <wayland-server-core.h>

namespace umbriel {

  struct Pacing::Impl {
    wl_display* display = nullptr;
    std::unique_ptr<FifoManager> fifoManager;
  };

  Pacing::Pacing(wl_display* display) : m_impl(std::make_unique<Impl>()) {
    m_impl->display = display;
    m_impl->fifoManager = std::make_unique<FifoManager>(display);
  }

  Pacing::~Pacing() = default;

  bool Pacing::valid() const { return m_impl->display != nullptr && m_impl->fifoManager->valid(); }

} // namespace umbriel
