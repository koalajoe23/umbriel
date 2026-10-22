#pragma once

#include <wayland-server-core.h>

struct wlr_compositor;

namespace umbriel {

  // wlroots holds a linux-drm-syncobj commit back only until its acquire fence materializes, so a commit applies while
  // the client's GPU work is still running. Direct scanout then hands KMS a buffer that is not ready, the page flip
  // waits on its fence, and the frame callback sent right after the commit lets a GPU-bound game queue its next frame
  // early: it presents in bursts (a short frame, then a long one) instead of at its render rate. This holds each such
  // commit until its acquire point has signalled, so a buffer reaches the scene only once it is ready to show.
  class SyncobjAcquireBarrier {
  public:
    explicit SyncobjAcquireBarrier(wlr_compositor* compositor);
    ~SyncobjAcquireBarrier();

    SyncobjAcquireBarrier(const SyncobjAcquireBarrier&) = delete;
    SyncobjAcquireBarrier& operator=(const SyncobjAcquireBarrier&) = delete;

  private:
    static void onNewSurface(wl_listener* listener, void* data);

    wl_listener m_newSurface{};
  };

} // namespace umbriel
