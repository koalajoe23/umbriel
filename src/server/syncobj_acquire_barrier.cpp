#include "server/syncobj_acquire_barrier.h"

#include "wlr.h"

#include <cstdint>
#include <vector>
#include <xf86drm.h>

namespace umbriel {

  namespace {

    struct SurfaceWatch;

    struct AcquireLock {
      wlr_drm_syncobj_timeline_waiter waiter{};
      SurfaceWatch* watch = nullptr;
      uint32_t seq = 0;
    };

    struct SurfaceWatch {
      wlr_surface* surface = nullptr;
      wl_event_loop* loop = nullptr;
      std::vector<AcquireLock*> locks;
      wl_listener clientCommit{};
      wl_listener destroy{};
    };

    void onAcquireSignalled(wlr_drm_syncobj_timeline_waiter* waiter) {
      AcquireLock* lock;
      lock = wl_container_of(waiter, lock, waiter);
      SurfaceWatch* watch = lock->watch;
      wlr_drm_syncobj_timeline_waiter_finish(&lock->waiter);
      std::erase(watch->locks, lock);
      const uint32_t seq = lock->seq;
      delete lock;
      // May apply the cached state, and with it every later one no other lock holds back.
      wlr_surface_unlock_cached(watch->surface, seq);
    }

    void onClientCommit(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, clientCommit);
      wlr_surface* surface = watch->surface;
      if ((surface->pending.committed & WLR_SURFACE_STATE_BUFFER) == 0 || surface->pending.buffer == nullptr) {
        return;
      }
      uint64_t point = 0;
      wlr_drm_syncobj_timeline* timeline =
          umbrielfx_linux_drm_syncobj_state_acquire_point(surface, &surface->pending, &point);
      if (timeline == nullptr) {
        return;
      }
      // WAIT_FOR_SUBMIT reports a point that has not materialized yet as unsignalled instead of failing.
      bool signalled = false;
      if (!wlr_drm_syncobj_timeline_check(timeline, point, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, &signalled)
          || signalled) {
        return;
      }
      auto* lock = new AcquireLock();
      lock->watch = watch;
      // No flags: wait for the point to signal, not just to materialize.
      if (!wlr_drm_syncobj_timeline_waiter_init(&lock->waiter, timeline, point, 0, watch->loop, onAcquireSignalled)) {
        delete lock;
        return;
      }
      lock->seq = wlr_surface_lock_pending(surface);
      watch->locks.push_back(lock);
    }

    void onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
      SurfaceWatch* watch;
      watch = wl_container_of(listener, watch, destroy);
      // Release the locks the way wlroots' own acquire wait does when the surface goes away.
      for (AcquireLock* lock : watch->locks) {
        wlr_drm_syncobj_timeline_waiter_finish(&lock->waiter);
        wlr_surface_unlock_cached(watch->surface, lock->seq);
        delete lock;
      }
      wl_list_remove(&watch->clientCommit.link);
      wl_list_remove(&watch->destroy.link);
      delete watch;
    }

  } // namespace

  SyncobjAcquireBarrier::SyncobjAcquireBarrier(wlr_compositor* compositor) {
    m_newSurface.notify = onNewSurface;
    wl_signal_add(&compositor->events.new_surface, &m_newSurface);
  }

  SyncobjAcquireBarrier::~SyncobjAcquireBarrier() { wl_list_remove(&m_newSurface.link); }

  void SyncobjAcquireBarrier::onNewSurface(wl_listener* /*listener*/, void* data) {
    auto* watch = new SurfaceWatch();
    watch->surface = static_cast<wlr_surface*>(data);
    watch->loop = wl_display_get_event_loop(wl_client_get_display(wl_resource_get_client(watch->surface->resource)));
    watch->clientCommit.notify = onClientCommit;
    wl_signal_add(&watch->surface->events.client_commit, &watch->clientCommit);
    watch->destroy.notify = onSurfaceDestroy;
    wl_signal_add(&watch->surface->events.destroy, &watch->destroy);
  }

} // namespace umbriel
