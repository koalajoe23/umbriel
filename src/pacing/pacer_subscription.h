#pragma once

#include <wayland-server-core.h>

struct wlr_surface;

namespace umbriel {

  class SurfacePacer;
  struct PacerRefreshEvent;

  // A protocol object's hold on its surface's SurfacePacer: forwards the pacer's refreshes to the owner until released,
  // and retains the pacer while the owner says it has work (setRetained), so a pacer with nothing to do emits nothing
  // and runs no timer. It holds at most one retain, and drops it on every way out. If the pacer goes first, the
  // subscription unlinks and never touches it again; pacer() is then null.
  class PacerSubscription {
  public:
    using RefreshHandler = void (*)(void* owner, const PacerRefreshEvent& event);

    PacerSubscription(RefreshHandler onRefresh, void* owner);
    ~PacerSubscription();

    PacerSubscription(const PacerSubscription&) = delete;
    PacerSubscription& operator=(const PacerSubscription&) = delete;

    // Finds or creates the surface's pacer and subscribes, without retaining it. False, with nothing held, when the
    // pacer cannot be allocated. Call it before adding the owner's own surface addon, so on surface teardown that
    // (newer) addon goes first and the pacer is still alive when the owner releases it.
    [[nodiscard]] bool subscribe(wlr_surface* surface);
    // Unsubscribes and drops the retain, if held. No-op when not subscribed or once the pacer is gone.
    void release();
    // Retains the pacer while `retained` is true, releases it once false. Idempotent; no-op when not subscribed.
    void setRetained(bool retained);

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
    bool m_retained = false;
    wl_listener m_refresh{};
    wl_listener m_destroy{};
  };

} // namespace umbriel
