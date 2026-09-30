#pragma once

#include <memory>

struct wl_display;

namespace umbriel {

  // The frame-pacing protocols (fifo-v1 so far), the only pacing type Server sees. It owns the protocol
  // managers; each surface's timing comes from its SurfacePacer.
  class Pacing {
  public:
    explicit Pacing(wl_display* display);
    ~Pacing();

    Pacing(const Pacing&) = delete;
    Pacing& operator=(const Pacing&) = delete;

    [[nodiscard]] bool valid() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
  };

} // namespace umbriel
