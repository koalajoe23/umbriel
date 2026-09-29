#pragma once

#include "pacing/commit_timing_policy.h"

#include <cstdint>
#include <memory>
#include <wayland-server-core.h>

struct umbrielfx_surface_pacing;
struct wl_event_loop;
struct wl_event_source;
struct wlr_output;
struct wlr_surface;

namespace umbriel {

  // One pacing instant for a surface: a frame done on its pacing output, or a hidden-surface tick.
  struct PacerRefreshEvent {
    Nanoseconds when; // CLOCK_MONOTONIC
    bool hidden;
    // For a frame done: the present a commit released at this refresh is expected to reach, predicted from the
    // presents recorded before the frame done (the frame's own present may already have arrived by the time the
    // refresh is emitted; headless sends it from an idle source). Equal to `when` for a hidden tick.
    Nanoseconds releasePresent;
    // This refresh's position among the pacer's refresh instants (see SurfacePacer::refreshSerial).
    std::uint64_t serial;
  };

  // Per-surface pacing mechanism shared by the fifo-v1 and commit-timing-v1 managers. It follows the umbrielfx
  // pacing addon's output, the output its wl_surface.frame callbacks are paced by, records that output's latest
  // present event for prediction, and emits one refresh signal: after each frame done for that output, and on a timer
  // while the surface is hidden and something retains the pacer. It knows nothing about either protocol.
  //
  // A frame done arrives inside the scene's frame-done tree walk, and a refresh listener may unlock commits whose
  // role handlers reparent or destroy scene nodes, so the refresh for a frame done is emitted from an idle source
  // once the walk has returned, never from within it.
  //
  // A pacer lives in a wlr_addon on the surface and is freed with the surface; events.destroy fires first so users
  // unlink their listeners.
  class SurfacePacer {
  public:
    // Finds the surface's pacer, creating it on first use.
    static SurfacePacer& from(wlr_surface* surface);

    SurfacePacer(const SurfacePacer&) = delete;
    SurfacePacer& operator=(const SurfacePacer&) = delete;

    // The surface's current pacing output, mirroring the umbrielfx addon; null while hidden.
    [[nodiscard]] wlr_output* output() const { return m_output; }
    // The pacing output's refresh period: its last present refresh, else its mode (the current output's, or the last
    // one's while hidden), else 60 Hz.
    [[nodiscard]] Nanoseconds period() const;
    // period() once the surface has had a pacing output; 0 until then.
    [[nodiscard]] Nanoseconds lastKnownPeriod() const;

    // The serial of the latest refresh instant: a frame done counts when it happens, not when its deferred refresh is
    // emitted, and a tick counts just before it is emitted. Content applied while this is S was not used by the
    // refresh with serial S.
    [[nodiscard]] std::uint64_t refreshSerial() const { return m_refreshSerial; }

    // Asks the pacing output for a frame, so a refresh arrives even when nothing else redraws. No-op while hidden.
    void requestFrame();
    // While at least one retain is outstanding and the surface is hidden, the hidden tick emits refresh.
    void retain();
    void release();

    struct {
      wl_signal refresh; // const PacerRefreshEvent*
      wl_signal destroy; // SurfacePacer*
    } events{};

  private:
    struct Addon;

    SurfacePacer(wlr_surface* surface, umbrielfx_surface_pacing* pacing);
    ~SurfacePacer();

    static void onOutputChange(wl_listener* listener, void* data);
    static void onFrameDone(wl_listener* listener, void* data);
    static void onPacingDestroy(wl_listener* listener, void* data);
    static void onOutputPresent(wl_listener* listener, void* data);
    static void onOutputDestroy(wl_listener* listener, void* data);
    static int onHiddenTick(void* data);
    static void onDeferredRefresh(void* data);

    void handleOutputChange();
    void handleFrameDone(void* data);
    void handleOutputPresent(void* data);
    void handleOutputDestroy();
    void handleHiddenTick();
    void handleDeferredRefresh();

    // Moves the present and destroy subscriptions to `output`, which becomes output().
    void setOutput(wlr_output* output);
    // Arms the hidden tick while retained and hidden, disarms it otherwise.
    void updateHiddenTick();
    void armHiddenTick();

    std::unique_ptr<Addon> m_addon;
    umbrielfx_surface_pacing* m_pacing = nullptr;
    wlr_output* m_output = nullptr;
    // The last output mode refresh seen, kept while hidden.
    int32_t m_lastOutputRefreshMhz = 0;
    // The pacing output's last present event; reset when the pacing output changes.
    Nanoseconds m_lastPresent = 0;
    Nanoseconds m_lastPresentRefresh = 0;
    bool m_hadOutput = false;
    uint32_t m_retainCount = 0;
    std::uint64_t m_refreshSerial = 0;
    wl_event_loop* m_eventLoop = nullptr;
    wl_event_source* m_hiddenTick = nullptr;
    bool m_hiddenTickArmed = false;
    // The idle source that emits the latest frame done's refresh, and that refresh; the source is null when none is
    // due.
    wl_event_source* m_deferredRefresh = nullptr;
    PacerRefreshEvent m_deferredEvent{};

    wl_listener m_outputChange{};
    wl_listener m_frameDone{};
    wl_listener m_pacingDestroy{};
    wl_listener m_outputPresent{};
    wl_listener m_outputDestroy{};
  };

} // namespace umbriel
