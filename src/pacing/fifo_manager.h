#pragma once

#include <memory>

struct wl_display;

namespace umbriel {

  // The wp_fifo_manager_v1 global and its wp_fifo_v1 objects: a commit that waits for a barrier is held until the
  // surface's next pacing refresh (SurfacePacer). A temporary in-tree implementation until wlroots ships fifo-v1.
  class FifoManager {
  public:
    explicit FifoManager(wl_display* display);
    ~FifoManager();

    FifoManager(const FifoManager&) = delete;
    FifoManager& operator=(const FifoManager&) = delete;

    [[nodiscard]] bool valid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
