#include "pacing/pacer_subscription.h"

#include "pacing/surface_pacer.h"

#include <new>

namespace umbriel {

  PacerSubscription::PacerSubscription(RefreshHandler onRefresh, void* owner) : m_onRefresh(onRefresh), m_owner(owner) {
    wl_list_init(&m_refresh.link);
    wl_list_init(&m_destroy.link);
  }

  PacerSubscription::~PacerSubscription() { release(); }

  bool PacerSubscription::subscribe(wlr_surface* surface) {
    SurfacePacer* pacer = nullptr;
    try {
      pacer = &SurfacePacer::from(surface);
    } catch (const std::bad_alloc&) {
      return false;
    }
    m_pacer = pacer;
    m_pacer->retain();
    m_refresh.notify = onRefresh;
    wl_signal_add(&m_pacer->events.refresh, &m_refresh);
    m_destroy.notify = onDestroy;
    wl_signal_add(&m_pacer->events.destroy, &m_destroy);
    return true;
  }

  void PacerSubscription::release() {
    if (m_pacer == nullptr) {
      return;
    }
    unlink();
    m_pacer->release();
    m_pacer = nullptr;
  }

  void PacerSubscription::onRefresh(wl_listener* listener, void* data) {
    PacerSubscription* self = nullptr;
    self = wl_container_of(listener, self, m_refresh);
    self->handleRefresh(*static_cast<const PacerRefreshEvent*>(data));
  }

  void PacerSubscription::onDestroy(wl_listener* listener, void* /*data*/) {
    PacerSubscription* self = nullptr;
    self = wl_container_of(listener, self, m_destroy);
    self->handleDestroy();
  }

  void PacerSubscription::handleRefresh(const PacerRefreshEvent& event) { m_onRefresh(m_owner, event); }

  // The pacer normally outlives its subscribers; if it goes first, stop listening and never touch it again. Never
  // calls SurfacePacer::from, which would recreate a pacer for a surface being torn down.
  void PacerSubscription::handleDestroy() {
    unlink();
    m_pacer = nullptr;
  }

  void PacerSubscription::unlink() {
    wl_list_remove(&m_refresh.link);
    wl_list_init(&m_refresh.link);
    wl_list_remove(&m_destroy.link);
    wl_list_init(&m_destroy.link);
  }

} // namespace umbriel
