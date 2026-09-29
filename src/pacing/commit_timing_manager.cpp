#include "pacing/commit_timing_manager.h"

#include "commit-timing-v1-server-protocol.h"
#include "pacing/commit_timing_policy.h"
#include "pacing/pacer_subscription.h"
#include "pacing/pacing_clock.h"
#include "pacing/protocol_global.h"
#include "pacing/surface_pacer.h"
#include "wlr.h"

#include <cstdint>
#include <deque>
#include <limits>
#include <new>
#include <optional>
#include <vector>

namespace umbriel {

  namespace {

    constexpr uint32_t kProtocolVersion = 1;

    // The commit-timing-v1 part of one content update, double-buffered with the surface state.
    struct TimingState {
      std::optional<Nanoseconds> target; // CLOCK_MONOTONIC
    };

    void initTimingState(void* state) { *static_cast<TimingState*>(state) = TimingState{}; }

    // Moving clears the source, so a later commit that does not set a timestamp again carries none.
    void moveTimingState(void* dst, void* src) {
      auto* from = static_cast<TimingState*>(src);
      *static_cast<TimingState*>(dst) = *from;
      *from = TimingState{};
    }

    constexpr wlr_surface_synced_impl kSyncedImpl = {
        .state_size = sizeof(TimingState),
        .init_state = initTimingState,
        .finish_state = nullptr,
        .move_state = moveTimingState,
        .commit = nullptr,
    };

    // The request's split seconds and nanoseconds as one CLOCK_MONOTONIC value, saturating at the far future.
    Nanoseconds timestampFrom(uint32_t secHi, uint32_t secLo, uint32_t nsec) {
      const uint64_t seconds = (static_cast<uint64_t>(secHi) << 32U) | secLo;
      constexpr auto kMaxSeconds = static_cast<uint64_t>((std::numeric_limits<Nanoseconds>::max() / kNsecPerSec) - 1);
      if (seconds > kMaxSeconds) {
        return std::numeric_limits<Nanoseconds>::max();
      }
      return (static_cast<Nanoseconds>(seconds) * kNsecPerSec) + nsec;
    }

    // One commit held for its timestamp.
    struct TimedCommit {
      std::uint32_t seq;
      Nanoseconds target;
    };

    // One surface's commit-timing state: its pending timestamp and the commits held for theirs, in commit order,
    // served to the client through a wp_commit_timer_v1. Destroying that resource leaves existing timing constraints
    // in force, so the state lingers, still releasing held commits by the normal rules, until its queue is empty or
    // the surface is gone; a new get_timer for the surface meanwhile takes it over. When the surface goes first, the
    // state drops its queue without unlocking and stays inert until the client destroys the resource.
    class CommitTimer {
    public:
      CommitTimer(wl_resource* resource, wlr_surface* surface)
          : m_resource(resource), m_surface(surface), m_pacer(onPacerRefresh, this) {}

      CommitTimer(const CommitTimer&) = delete;
      CommitTimer& operator=(const CommitTimer&) = delete;
      ~CommitTimer() = default;

      // Hooks the timer into its surface. False, with nothing hooked, when the synced state or the event source
      // cannot be allocated.
      bool attach() {
        // Retained while the state lives, so a hidden surface keeps ticking and its queue keeps draining;
        // subscribed before the addon is added (see PacerSubscription::subscribe).
        if (!m_pacer.subscribe(m_surface)) {
          return false;
        }
        wl_display* display = wl_client_get_display(wl_resource_get_client(m_surface->resource));
        m_timer = wl_event_loop_add_timer(wl_display_get_event_loop(display), onTimer, this);
        if (m_timer == nullptr) {
          m_pacer.release();
          return false;
        }
        if (!wlr_surface_synced_init(&m_synced, m_surface, &kSyncedImpl, &m_pending, &m_current)) {
          wl_event_source_remove(m_timer);
          m_timer = nullptr;
          m_pacer.release();
          return false;
        }

        wlr_addon_init(&m_addon, &m_surface->addons, &kAddonInterface, &kAddonInterface);
        m_clientCommit.notify = onClientCommit;
        wl_signal_add(&m_surface->events.client_commit, &m_clientCommit);
        return true;
      }

      // The surface's timing state, live or lingering, if it has one.
      [[nodiscard]] static CommitTimer* fromSurface(wlr_surface* surface) {
        wlr_addon* addon = wlr_addon_find(&surface->addons, &kAddonInterface, &kAddonInterface);
        if (addon == nullptr) {
          return nullptr;
        }
        CommitTimer* timer = nullptr;
        timer = wl_container_of(addon, timer, m_addon);
        return timer;
      }

      [[nodiscard]] bool hasResource() const { return m_resource != nullptr; }

      // A lingering state is served through a new wp_commit_timer_v1.
      void bindResource(wl_resource* resource) { m_resource = resource; }

      static CommitTimer* fromResource(wl_resource* resource) {
        return static_cast<CommitTimer*>(wl_resource_get_user_data(resource));
      }

      static const struct wp_commit_timer_v1_interface kImplementation;

      static void
      handleSetTimestamp(wl_client* /*client*/, wl_resource* resource, uint32_t secHi, uint32_t secLo, uint32_t nsec) {
        CommitTimer* timer = fromResource(resource);
        if (timer->m_surface == nullptr) {
          wl_resource_post_error(
              resource, WP_COMMIT_TIMER_V1_ERROR_SURFACE_DESTROYED, "the wp_commit_timer_v1's surface no longer exists"
          );
          return;
        }
        if (nsec >= kNsecPerSec) {
          wl_resource_post_error(resource, WP_COMMIT_TIMER_V1_ERROR_INVALID_TIMESTAMP, "tv_nsec is out of range");
          return;
        }
        if (timer->m_pending.target.has_value()) {
          wl_resource_post_error(
              resource, WP_COMMIT_TIMER_V1_ERROR_TIMESTAMP_EXISTS, "the pending commit already has a timestamp"
          );
          return;
        }
        timer->m_pending.target = timestampFrom(secHi, secLo, nsec);
      }

      // The resource is gone, by request or with its client. The protocol leaves existing timing constraints
      // unaffected, so held commits (and a timestamp set for the next commit) keep the state alive until they are
      // released by the normal rules.
      static void handleResourceDestroy(wl_resource* resource) {
        CommitTimer* timer = fromResource(resource);
        timer->m_resource = nullptr;
        timer->freeIfDone();
      }

    private:
      static void onAddonDestroy(wlr_addon* addon) {
        CommitTimer* timer = nullptr;
        timer = wl_container_of(addon, timer, m_addon);
        timer->handleSurfaceDestroy();
      }

      static constexpr wlr_addon_interface kAddonInterface = {
          .name = "umbriel_wp_commit_timer_v1",
          .destroy = onAddonDestroy,
      };

      static void onClientCommit(wl_listener* listener, void* /*data*/) {
        CommitTimer* timer = nullptr;
        timer = wl_container_of(listener, timer, m_clientCommit);
        timer->handleClientCommit();
      }

      // May free the state (freeIfDone); the subscription touches nothing after forwarding.
      static void onPacerRefresh(void* owner, const PacerRefreshEvent& event) {
        static_cast<CommitTimer*>(owner)->handlePacerRefresh(event);
      }

      static int onTimer(void* data) {
        static_cast<CommitTimer*>(data)->handleTimer();
        return 0;
      }

      [[nodiscard]] bool visible() const { return m_pacer.pacer() != nullptr && m_pacer.pacer()->output() != nullptr; }

      // A commit with a timestamp is held for it, behind any older held commit. A synchronized subsurface's commit is
      // held too: the protocol makes no exception, and wlroots applies the cached state only once both the parent's
      // commit and this lock have released it.
      void handleClientCommit() {
        if (!m_pending.target.has_value()) {
          return;
        }
        const std::uint32_t seq = wlr_surface_lock_pending(m_surface);
        m_queue.push_back(TimedCommit{.seq = seq, .target = *m_pending.target});
        schedule();
      }

      // A visible refresh releases held commits, from the head, while the present a commit released now would reach
      // (`now` itself when the last present is stale, so an idle output never releases early) is due for its target;
      // a hidden tick releases those whose target has passed. The pacing output changing
      // does not flush the queue: it simply follows the new output's refreshes.
      void handlePacerRefresh(const PacerRefreshEvent& event) {
        if (event.hidden) {
          releaseWhile([&](Nanoseconds target) { return target <= event.when; });
        } else {
          // A visible refresh comes from the pacer, so it is there.
          const SurfacePacer& pacer = *m_pacer.pacer();
          const Nanoseconds period = pacer.period();
          releaseWhile([&](Nanoseconds target) { return timedCommitDue(target, event.releasePresent, period); });
        }
        if (freeIfDone()) {
          return;
        }
        // A refresh that leaves the head held inside its wakeup range asks for the next frame, so an idle output keeps
        // refreshing until the head is due.
        schedule();
      }

      // While visible the timer only asks for the frames whose refreshes release the head; while hidden, with no
      // frames to wait for, it releases the head itself once its target has passed.
      void handleTimer() {
        m_timerArmed = false;
        if (visible()) {
          m_pacer.pacer()->requestFrame();
          return;
        }
        const Nanoseconds now = monotonicNow();
        releaseWhile([&](Nanoseconds target) { return target <= now; });
        if (freeIfDone()) {
          return;
        }
        schedule();
      }

      // The surface is being destroyed: its cached states, held commits included, go with it, so the queue is
      // dropped without unlocking anything.
      void handleSurfaceDestroy() {
        m_queue.clear();
        detach();
        if (m_resource == nullptr) {
          delete this;
        }
      }

      // Frees a state whose resource is gone once nothing it holds or has pending still needs it. True when freed;
      // the caller must not touch the timer afterwards.
      bool freeIfDone() {
        if (m_resource != nullptr) {
          return false;
        }
        if (m_surface != nullptr) {
          if (!m_queue.empty() || m_pending.target.has_value()) {
            return false;
          }
          detach();
        }
        delete this;
        return true;
      }

      // Releases held commits from the head while `due(target)` holds. Only the head is ever released, since
      // wlroots applies cached states in commit order anyway.
      template <typename Due> void releaseWhile(Due due) {
        std::vector<std::uint32_t> released;
        while (!m_queue.empty() && due(m_queue.front().target)) {
          released.push_back(m_queue.front().seq);
          m_queue.pop_front();
        }
        // Unlocking can apply commits synchronously; the queue is already updated, and the timer has no commit
        // listener to re-enter.
        for (const std::uint32_t seq : released) {
          wlr_surface_unlock_cached(m_surface, seq);
        }
      }

      // Arms the timer for the head of the queue: while visible, at the head's frame wakeup, or straight away asks
      // for a frame once that has passed; while hidden, at the head's target. Disarms it when nothing is held.
      void schedule() {
        if (m_queue.empty()) {
          disarmTimer();
          return;
        }
        const Nanoseconds now = monotonicNow();
        const Nanoseconds target = m_queue.front().target;
        if (!visible()) {
          armTimer(target - now);
          return;
        }
        SurfacePacer& pacer = *m_pacer.pacer();
        const Nanoseconds wakeup = frameWakeup(target, pacer.period());
        if (wakeup <= now) {
          disarmTimer();
          pacer.requestFrame();
          return;
        }
        armTimer(wakeup - now);
      }

      void armTimer(Nanoseconds delay) {
        wl_event_source_timer_update(m_timer, timerDelayMsecNotBefore(delay));
        m_timerArmed = true;
      }

      void disarmTimer() {
        if (m_timerArmed) {
          wl_event_source_timer_update(m_timer, 0);
          m_timerArmed = false;
        }
      }

      // Unhooks the timer from its surface, leaving it inert.
      void detach() {
        m_pacer.release();
        wl_event_source_remove(m_timer);
        m_timer = nullptr;
        m_timerArmed = false;
        wl_list_remove(&m_clientCommit.link);
        wlr_addon_finish(&m_addon);
        wlr_surface_synced_finish(&m_synced);
        m_surface = nullptr;
      }

      wl_resource* m_resource = nullptr;
      wlr_surface* m_surface = nullptr;
      PacerSubscription m_pacer;
      std::deque<TimedCommit> m_queue;
      wl_event_source* m_timer = nullptr;
      bool m_timerArmed = false;
      wlr_addon m_addon{};
      wlr_surface_synced m_synced{};
      TimingState m_pending;
      TimingState m_current;
      wl_listener m_clientCommit{};
    };

    const struct wp_commit_timer_v1_interface CommitTimer::kImplementation = {
        .set_timestamp = CommitTimer::handleSetTimestamp,
        .destroy = handleDestroyRequest,
    };

  } // namespace

  struct CommitTimingManager::Impl {
    explicit Impl(wl_display* display) : global(display, kGlobalSpec) {}

    static const struct wp_commit_timing_manager_v1_interface kImplementation;
    static const ProtocolGlobalSpec kGlobalSpec;

    ProtocolGlobal global;

    static void handleGetTimer(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surfaceResource) {
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      CommitTimer* lingering = CommitTimer::fromSurface(surface);
      if (lingering != nullptr && lingering->hasResource()) {
        wl_resource_post_error(
            manager, WP_COMMIT_TIMING_MANAGER_V1_ERROR_COMMIT_TIMER_EXISTS,
            "the surface already has a wp_commit_timer_v1 object"
        );
        return;
      }
      wl_resource* resource =
          wl_resource_create(client, &wp_commit_timer_v1_interface, wl_resource_get_version(manager), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      if (lingering != nullptr) {
        // The surface's state outlived its previous resource; the new one takes it over, held commits included.
        lingering->bindResource(resource);
        wl_resource_set_implementation(
            resource, &CommitTimer::kImplementation, lingering, CommitTimer::handleResourceDestroy
        );
        return;
      }
      auto* timer = new (std::nothrow) CommitTimer(resource, surface);
      if (timer == nullptr || !timer->attach()) {
        delete timer;
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
      }
      // Freed once both the resource and what it holds are gone (CommitTimer::freeIfDone), or with the surface.
      wl_resource_set_implementation(
          resource, &CommitTimer::kImplementation, timer, CommitTimer::handleResourceDestroy
      );
    }
  };

  const struct wp_commit_timing_manager_v1_interface CommitTimingManager::Impl::kImplementation = {
      .destroy = handleDestroyRequest,
      .get_timer = CommitTimingManager::Impl::handleGetTimer,
  };

  const ProtocolGlobalSpec CommitTimingManager::Impl::kGlobalSpec = {
      .interface = &wp_commit_timing_manager_v1_interface,
      .version = kProtocolVersion,
      .implementation = &CommitTimingManager::Impl::kImplementation,
  };

  CommitTimingManager::CommitTimingManager(wl_display* display) : m_impl(std::make_unique<Impl>(display)) {}

  CommitTimingManager::~CommitTimingManager() = default;

  bool CommitTimingManager::valid() const { return m_impl->global.valid(); }

} // namespace umbriel
