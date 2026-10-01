#include "pacing/fifo_manager.h"

#include "core/log.h"
#include "core/tracy.h"
#include "fifo-v1-server-protocol.h"
#include "pacing/fifo_queue.h"
#include "pacing/pacer_subscription.h"
#include "pacing/protocol_global.h"
#include "pacing/surface_pacer.h"
#include "wlr.h"

#include <cstdint>
#include <new>
#include <vector>

namespace umbriel {

  namespace {

    constexpr Logger kLog("pacing");

    constexpr uint32_t kProtocolVersion = 1;

    // The fifo-v1 part of one content update, double-buffered with the surface state.
    struct FifoState {
      bool setBarrier = false;
      bool waitBarrier = false;
    };

    void initFifoState(void* state) { *static_cast<FifoState*>(state) = FifoState{}; }

    // Moving clears the source, so a later commit that does not repeat the requests carries neither.
    void moveFifoState(void* dst, void* src) {
      auto* from = static_cast<FifoState*>(src);
      *static_cast<FifoState*>(dst) = *from;
      *from = FifoState{};
    }

    constexpr wlr_surface_synced_impl kSyncedImpl = {
        .state_size = sizeof(FifoState),
        .init_state = initFifoState,
        .finish_state = nullptr,
        .move_state = moveFifoState,
        .commit = nullptr,
    };

    bool isSynchronizedSubsurface(wlr_surface* surface) {
      const wlr_subsurface* subsurface = wlr_subsurface_try_from_wlr_surface(surface);
      return subsurface != nullptr && subsurface->synchronized;
    }

    // One wp_fifo_v1: its surface's barrier and the commits held behind it. It is freed with its resource. When the
    // surface goes first, the fifo drops its queue without unlocking and stays inert until the client destroys it.
    class Fifo {
    public:
      Fifo(wl_resource* resource, wlr_surface* surface)
          : m_resource(resource), m_surface(surface), m_pacer(onPacerRefresh, this) {}

      Fifo(const Fifo&) = delete;
      Fifo& operator=(const Fifo&) = delete;
      ~Fifo() = default;

      // Hooks the fifo into its surface. False, with nothing hooked, when the synced state cannot be allocated.
      bool attach() {
        // Subscribed before the addon is added (see PacerSubscription::subscribe); retained only while the queue has
        // work (updateRetain), so a hidden, idle surface runs no tick.
        if (!m_pacer.subscribe(m_surface)) {
          return false;
        }
        if (!wlr_surface_synced_init(&m_synced, m_surface, &kSyncedImpl, &m_pending, &m_current)) {
          m_pacer.release();
          return false;
        }

        wlr_addon_init(&m_addon, &m_surface->addons, &kAddonInterface, &kAddonInterface);
        m_clientCommit.notify = onClientCommit;
        wl_signal_add(&m_surface->events.client_commit, &m_clientCommit);
        m_commit.notify = onCommit;
        wl_signal_add(&m_surface->events.commit, &m_commit);
        return true;
      }

      [[nodiscard]] static bool surfaceHasFifo(wlr_surface* surface) {
        return wlr_addon_find(&surface->addons, &kAddonInterface, &kAddonInterface) != nullptr;
      }

      static Fifo* fromResource(wl_resource* resource) {
        return static_cast<Fifo*>(wl_resource_get_user_data(resource));
      }

      static const struct wp_fifo_v1_interface kImplementation;

      static void handleSetBarrier(wl_client* /*client*/, wl_resource* resource) {
        Fifo* fifo = fromResource(resource);
        if (fifo->m_surface == nullptr) {
          fifo->postSurfaceDestroyed();
          return;
        }
        fifo->m_pending.setBarrier = true;
      }

      static void handleWaitBarrier(wl_client* /*client*/, wl_resource* resource) {
        Fifo* fifo = fromResource(resource);
        if (fifo->m_surface == nullptr) {
          fifo->postSurfaceDestroyed();
          return;
        }
        fifo->m_pending.waitBarrier = true;
      }

      // The resource is gone, by request or with its client: every held commit is released, as the protocol leaves
      // earlier content updates unaffected by the fifo's destruction.
      static void handleResourceDestroy(wl_resource* resource) {
        Fifo* fifo = fromResource(resource);
        wlr_surface* surface = fifo->m_surface;
        if (surface != nullptr) {
          const std::vector<std::uint32_t> released = fifo->m_queue.drain();
          kLog.debug(
              "wp_fifo_v1 destroyed for surface {}, releasing {} held commits", static_cast<const void*>(surface),
              released.size()
          );
          fifo->detach();
          // Unlocking can apply commits synchronously; the fifo no longer listens, so none reach it.
          for (const std::uint32_t seq : released) {
            wlr_surface_unlock_cached(surface, seq);
          }
        }
        delete fifo;
      }

    private:
      static void onAddonDestroy(wlr_addon* addon) {
        Fifo* fifo = nullptr;
        fifo = wl_container_of(addon, fifo, m_addon);
        fifo->handleSurfaceDestroy();
      }

      static constexpr wlr_addon_interface kAddonInterface = {
          .name = "umbriel_wp_fifo_v1",
          .destroy = onAddonDestroy,
      };

      static void onClientCommit(wl_listener* listener, void* /*data*/) {
        Fifo* fifo = nullptr;
        fifo = wl_container_of(listener, fifo, m_clientCommit);
        fifo->handleClientCommit();
      }

      static void onCommit(wl_listener* listener, void* /*data*/) {
        Fifo* fifo = nullptr;
        fifo = wl_container_of(listener, fifo, m_commit);
        fifo->handleCommit();
      }

      static void onPacerRefresh(void* owner, const PacerRefreshEvent& event) {
        static_cast<Fifo*>(owner)->handlePacerRefresh(event);
      }

      // A commit that waits for the barrier while one is set, while a setter ahead of it has not applied yet, or while
      // older commits are still held, is held too. Every commit is recorded, so setters are tracked until they apply.
      void handleClientCommit() {
        UMBRIEL_ZONE("Fifo::handleClientCommit");
        const bool hold = m_queue.shouldHold(m_pending.waitBarrier, isSynchronizedSubsurface(m_surface));
        if (hold) {
          const std::uint32_t seq = wlr_surface_lock_pending(m_surface);
          m_queue.hold(HeldCommit{.seq = seq, .setsBarrier = m_pending.setBarrier});
        }
        m_queue.committed(m_pending.setBarrier);
        // Makes sure a refresh comes even when nothing else would draw the pacing output.
        if (SurfacePacer* pacer = m_pacer.pacer(); hold && pacer != nullptr) {
          pacer->requestFrame();
        }
        updateRetain();
      }

      void handleCommit() {
        if (m_current.setBarrier) {
          SurfacePacer* pacer = m_pacer.pacer();
          m_barrierSerial = pacer != nullptr ? pacer->refreshSerial() : 0;
        }
        m_queue.applied(m_current.setBarrier);
        updateRetain();
      }

      // A refresh clears the barrier and releases held commits up to the next one that sets it again. A barrier set
      // at or after this refresh's instant (applied between a frame done and its deferred refresh, or by another
      // listener of this very refresh, such as commit-timing releasing the setter) was not used by it, so the refresh
      // does not count for it. The pacing output changing does not flush the queue: it simply follows the new
      // output's refreshes.
      void handlePacerRefresh(const PacerRefreshEvent& event) {
        UMBRIEL_ZONE("Fifo::handlePacerRefresh");
        if (m_queue.barrier() && m_barrierSerial >= event.serial) {
          if (SurfacePacer* pacer = m_pacer.pacer()) {
            pacer->requestFrame();
          }
          return;
        }
        const std::vector<std::uint32_t> released = m_queue.refresh();
        // Unlocking can apply a commit synchronously, which re-enters handleCommit and re-arms the barrier.
        for (const std::uint32_t seq : released) {
          wlr_surface_unlock_cached(m_surface, seq);
        }
        SurfacePacer* pacer = m_pacer.pacer();
        if (!m_queue.empty() && pacer != nullptr) {
          pacer->requestFrame();
        }
        UMBRIEL_PLOT("fifo-v1 held commits", static_cast<int64_t>(m_queue.size()));
        updateRetain();
      }

      // The pacer ticks (hidden) and watches for stalls (visible) only while a refresh has something to do here.
      void updateRetain() { m_pacer.setRetained(!m_queue.idle()); }

      // The surface is being destroyed: its cached states, held commits included, go with it, so the queue is
      // dropped without unlocking anything.
      void handleSurfaceDestroy() {
        kLog.debug(
            "surface {} destroyed under its wp_fifo_v1 with {} held commits", static_cast<const void*>(m_surface),
            m_queue.size()
        );
        m_queue = FifoQueue{};
        detach();
      }

      // Unhooks the fifo from its surface, leaving it inert.
      void detach() {
        m_pacer.release();
        wl_list_remove(&m_clientCommit.link);
        wl_list_remove(&m_commit.link);
        wlr_addon_finish(&m_addon);
        wlr_surface_synced_finish(&m_synced);
        m_surface = nullptr;
      }

      void postSurfaceDestroyed() {
        kLog.debug("wp_fifo_v1 request after its surface was destroyed: posting surface_destroyed");
        wl_resource_post_error(
            m_resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the wp_fifo_v1's surface no longer exists"
        );
      }

      wl_resource* m_resource = nullptr;
      wlr_surface* m_surface = nullptr;
      PacerSubscription m_pacer;
      FifoQueue m_queue;
      // The pacer's refresh serial when the barrier was last set (see handlePacerRefresh).
      std::uint64_t m_barrierSerial = 0;
      wlr_addon m_addon{};
      wlr_surface_synced m_synced{};
      FifoState m_pending;
      FifoState m_current;
      wl_listener m_clientCommit{};
      wl_listener m_commit{};
    };

    const struct wp_fifo_v1_interface Fifo::kImplementation = {
        .set_barrier = Fifo::handleSetBarrier,
        .wait_barrier = Fifo::handleWaitBarrier,
        .destroy = handleDestroyRequest,
    };

  } // namespace

  struct FifoManager::Impl {
    explicit Impl(wl_display* display) : global(display, kGlobalSpec) {}

    static const struct wp_fifo_manager_v1_interface kImplementation;
    static const ProtocolGlobalSpec kGlobalSpec;

    ProtocolGlobal global;

    static void handleGetFifo(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surfaceResource) {
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      if (Fifo::surfaceHasFifo(surface)) {
        kLog.debug("second wp_fifo_v1 for surface {}: posting already_exists", static_cast<const void*>(surface));
        wl_resource_post_error(
            manager, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS, "the surface already has a wp_fifo_v1 object"
        );
        return;
      }
      wl_resource* resource = wl_resource_create(client, &wp_fifo_v1_interface, wl_resource_get_version(manager), id);
      if (resource == nullptr) {
        kLog.error("out of memory creating a wp_fifo_v1 for surface {}", static_cast<const void*>(surface));
        wl_client_post_no_memory(client);
        return;
      }
      auto* fifo = new (std::nothrow) Fifo(resource, surface);
      if (fifo == nullptr || !fifo->attach()) {
        kLog.error("out of memory creating a wp_fifo_v1 for surface {}", static_cast<const void*>(surface));
        delete fifo;
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
      }
      // Owned by the resource; freed by Fifo::handleResourceDestroy.
      wl_resource_set_implementation(resource, &Fifo::kImplementation, fifo, Fifo::handleResourceDestroy);
      kLog.debug("wp_fifo_v1 created for surface {}", static_cast<const void*>(surface));
    }
  };

  const struct wp_fifo_manager_v1_interface FifoManager::Impl::kImplementation = {
      .destroy = handleDestroyRequest,
      .get_fifo = FifoManager::Impl::handleGetFifo,
  };

  const ProtocolGlobalSpec FifoManager::Impl::kGlobalSpec = {
      .interface = &wp_fifo_manager_v1_interface,
      .version = kProtocolVersion,
      .implementation = &FifoManager::Impl::kImplementation,
  };

  FifoManager::FifoManager(wl_display* display) : m_impl(std::make_unique<Impl>(display)) {}

  FifoManager::~FifoManager() = default;

  bool FifoManager::valid() const { return m_impl->global.valid(); }

} // namespace umbriel
