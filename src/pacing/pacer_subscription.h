#pragma once

#include <wayland-server-core.h>

struct wlr_surface;

namespace umbriel {

  class SurfacePacer;
  struct PacerRefreshEvent;

  // A protocol object's hold on its surface's SurfacePacer: retains the pacer, so a hidden surface keeps ticking,
  // and forwards its refreshes to the owner until released. If the pacer goes first, the subscription unlinks and
  // never touches it again; pacer() is then null.
  class PacerSubscription {
  public:
    using RefreshHandler = void (*)(void* owner, const PacerRefreshEvent& event);

    PacerSubscription(RefreshHandler onRefresh, void* owner);
    ~PacerSubscription();

    PacerSubscription(const PacerSubscription&) = delete;
    PacerSubscription& operator=(const PacerSubscription&) = delete;

    // Finds or creates the surface's pacer, retains it, and subscribes. False, with nothing held, when the pacer
    // cannot be allocated. Call it before adding the owner's own surface addon, so on surface teardown that (newer)
    // addon goes first and the pacer is still alive when the owner releases it.
    [[nodiscard]] bool subscribe(wlr_surface* surface);
    // Unsubscribes and drops the retain. No-op when not subscribed or once the pacer is gone.
    void release();

    [[nodiscard]] SurfacePacer* pacer() const { return m_pacer; }

  private:
    static void onRefresh(wl_listener* listener, void* data);
    static void onDestroy(wl_listener* listener, void* data);

    void handleRefresh(const PacerRefreshEvent& event);
    void handleDestroy();

    void unlink();

    RefreshHandler m_onRefresh;
    void* m_owner;
    SurfacePacer* m_pacer = nullptr;
    wl_listener m_refresh{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
