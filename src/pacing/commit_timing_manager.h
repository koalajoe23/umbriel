#pragma once

#include <memory>

struct wl_display;

namespace umbriel {

  // The wp_commit_timing_manager_v1 global and its wp_commit_timer_v1 objects: a commit that carries a timestamp is
  // held until the surface's pacing refresh (SurfacePacer) whose present reaches that target.
  class CommitTimingManager {
  public:
    explicit CommitTimingManager(wl_display* display);
    ~CommitTimingManager();

    CommitTimingManager(const CommitTimingManager&) = delete;
    CommitTimingManager& operator=(const CommitTimingManager&) = delete;

    [[nodiscard]] bool valid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
