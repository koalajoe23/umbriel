#include "pacing/surface_pacer.h"

#include "check.h"
#include "pacing/commit_timing_policy.h"
#include "pacing/pacing_clock.h"

#include <ctime>
#include <optional>
#include <sys/socket.h>
#include <unistd.h>

// clang-format off
#include "wlr.h"
// clang-format on

// SurfacePacer snapshots, at each frame done, which present a commit released there is expected to reach. Whether the
// output's flip is still pending decides it: a pending flip pushes the released commit a period further out. The
// pure prediction is pinned in commit_timing.cpp; this pins that the pacer feeds it the output's real frame_pending.
// Headless harness timing cannot tell the two apart, so every instant here is chosen by hand and no clock is read on
// the path under test.

namespace {

  using umbriel::kNsecPerSec;
  using umbriel::Nanoseconds;
  using umbriel::PacerRefreshEvent;
  using umbriel::SurfacePacer;

  constexpr Nanoseconds kLastPresent = 1'000'000'000;
  constexpr Nanoseconds kPeriod = 10'000'000;
  constexpr Nanoseconds kFreshFrameDone = 1'005'000'000;
  constexpr Nanoseconds kStaleFrameDone = 1'025'000'000;

  timespec toTimespec(Nanoseconds nsec) {
    return timespec{
        .tv_sec = static_cast<time_t>(nsec / kNsecPerSec), .tv_nsec = static_cast<long>(nsec % kNsecPerSec)
    };
  }

  struct RefreshRecorder {
    wl_listener listener{};
    std::optional<PacerRefreshEvent> last;
    int count = 0;

    static void handle(wl_listener* listener, void* data) {
      RefreshRecorder* self = nullptr;
      self = wl_container_of(listener, self, listener);
      self->last = *static_cast<const PacerRefreshEvent*>(data);
      ++self->count;
    }
  };

  // A headless output in a scene, and a hand-built wlr_surface in that scene whose umbrielfx pacing output is set
  // through the real scene outputs_update path, as umbrielfx/tests/pacing_hook.c does. Only the surface fields the
  // scene-surface listeners, the umbrielfx addon, and the pacer touch are set up; a wlroots bump can add more.
  struct Fixture {
    wl_display* display = nullptr;
    int sockets[2] = {-1, -1};
    wl_client* client = nullptr;
    wlr_backend* backend = nullptr;
    wlr_output* output = nullptr;
    wlr_scene* scene = nullptr;
    wlr_scene_output* sceneOutput = nullptr;
    wlr_surface surface{};
    bool surfaceReady = false;
    wlr_scene_surface* sceneSurface = nullptr;
    umbrielfx_surface_pacing* pacing = nullptr;
    SurfacePacer* pacer = nullptr;
    RefreshRecorder recorder;

    [[nodiscard]] bool setUp() {
      display = wl_display_create();
      if (display == nullptr || socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0) {
        return false;
      }
      client = wl_client_create(display, sockets[0]);
      backend = wlr_headless_backend_create(wl_display_get_event_loop(display));
      scene = wlr_scene_create();
      if (client == nullptr || backend == nullptr || scene == nullptr) {
        return false;
      }
      output = wlr_headless_add_output(backend, 800, 600);
      if (output == nullptr) {
        return false;
      }
      sceneOutput = wlr_scene_output_create(scene, output);
      if (sceneOutput == nullptr) {
        return false;
      }

      surface.resource = wl_resource_create(client, &wl_surface_interface, 1, 0);
      if (surface.resource == nullptr) {
        return false;
      }
      wl_list_init(&surface.current_outputs);
      wl_list_init(&surface.current.frame_callback_list);
      wl_signal_init(&surface.events.destroy);
      wl_signal_init(&surface.events.commit);
      wlr_addon_set_init(&surface.addons);
      pixman_region32_init(&surface.opaque_region);
      surfaceReady = true;

      sceneSurface = wlr_scene_surface_create(&scene->tree, &surface);
      if (sceneSurface == nullptr) {
        return false;
      }
      pacer = &SurfacePacer::from(&surface);
      pacing = umbrielfx_surface_pacing_get(&surface);

      wlr_scene_output* active = sceneOutput;
      wlr_scene_outputs_update_event update{.active = &active, .size = 1};
      wl_signal_emit_mutable(&sceneSurface->buffer->events.outputs_update, &update);
      if (pacing->output != output || pacer->output() != output) {
        return false;
      }

      recorder.listener.notify = RefreshRecorder::handle;
      wl_signal_add(&pacer->events.refresh, &recorder.listener);
      return true;
    }

    void tearDown() {
      if (recorder.listener.notify != nullptr) {
        wl_list_remove(&recorder.listener.link);
      }
      if (surfaceReady) {
        // The scene surface follows the surface's destroy; the addon set then frees the pacer and the umbrielfx hook.
        wl_signal_emit_mutable(&surface.events.destroy, &surface);
        pixman_region32_fini(&surface.opaque_region);
        wlr_addon_set_finish(&surface.addons);
      }
      if (scene != nullptr) {
        wlr_scene_node_destroy(&scene->tree.node);
      }
      if (backend != nullptr) {
        wlr_backend_destroy(backend);
      }
      if (display != nullptr) {
        wl_display_destroy_clients(display);
        wl_display_destroy(display);
      }
      // sockets[0] belongs to the client once it exists, and wl_display_destroy_clients closed it.
      if (client == nullptr && sockets[0] >= 0) {
        close(sockets[0]);
      }
      if (sockets[1] >= 0) {
        close(sockets[1]);
      }
    }

    void present(Nanoseconds when, Nanoseconds refresh) {
      wlr_output_event_present event{};
      event.output = output;
      event.presented = true;
      event.when = toTimespec(when);
      event.refresh = static_cast<int>(refresh);
      wl_signal_emit_mutable(&output->events.present, &event);
    }

    // Seeds the last present, sets the output's flip state, and runs one retained frame done through to its deferred
    // refresh.
    std::optional<PacerRefreshEvent> refreshAfterFrameDone(bool framePending, Nanoseconds frameDone) {
      present(kLastPresent, kPeriod);
      output->frame_pending = framePending;
      pacer->retain();
      const timespec when = toTimespec(frameDone);
      umbrielfx_surface_pacing_frame_event event{.output = output, .when = &when};
      wl_signal_emit_mutable(&pacing->events.frame_done, &event);
      wl_event_loop_dispatch_idle(wl_display_get_event_loop(display));
      pacer->release();
      output->frame_pending = false;
      return recorder.count == 1 ? recorder.last : std::nullopt;
    }
  };

  void checkRelease(bool framePending, Nanoseconds frameDone, Nanoseconds expected) {
    Fixture fixture;
    CHECK(fixture.setUp());
    if (fixture.pacer != nullptr && fixture.pacing != nullptr && fixture.pacer->output() == fixture.output) {
      const std::optional<PacerRefreshEvent> refresh = fixture.refreshAfterFrameDone(framePending, frameDone);
      CHECK(refresh.has_value());
      if (refresh) {
        CHECK_EQ(refresh->when, frameDone);
        CHECK(!refresh->hidden);
        CHECK_EQ(refresh->releasePresent, expected);
      }
    }
    fixture.tearDown();
  }

} // namespace

// The frame behind the refresh committed and its flip is pending: the released commit renders at the next frame event
// and presents the period after it.
UMBRIEL_TEST(pendingFlipPredictsTheFollowingPresent) { checkRelease(true, kFreshFrameDone, 1'020'000'000); }

// The frame committed nothing: the released commit gets a frame straight away and flips at the very next vblank.
UMBRIEL_TEST(idleFramePredictsTheNextPresent) { checkRelease(false, kFreshFrameDone, 1'010'000'000); }

// Past kPredictionFreshPeriods the last present is not extrapolated, pending flip or not.
UMBRIEL_TEST(staleLastPresentPredictsTheFrameDoneItself) {
  checkRelease(true, kStaleFrameDone, kStaleFrameDone);
  checkRelease(false, kStaleFrameDone, kStaleFrameDone);
}

int main() { return RUN_TESTS(); }
