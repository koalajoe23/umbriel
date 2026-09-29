#include "pacing/fifo_manager.h"

#include "fifo-v1-server-protocol.h"
#include "pacing/fifo_queue.h"
#include "pacing/surface_pacer.h"
#include "wlr.h"

#include <algorithm>
#include <cstdint>
#include <new>
#include <vector>

namespace umbriel {

  namespace {

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
      Fifo(wl_resource* resource, wlr_surface* surface) : m_resource(resource), m_surface(surface) {
        wl_list_init(&m_pacerRefresh.link);
        wl_list_init(&m_pacerDestroy.link);
      }

      Fifo(const Fifo&) = delete;
      Fifo& operator=(const Fifo&) = delete;
      ~Fifo() = default;

      // Hooks the fifo into its surface. False, with nothing hooked, when the synced state cannot be allocated.
      bool attach() {
        // The pacer is found or created before the addon, so on surface teardown this (newer) addon goes first
        // and the pacer is still alive when it is released.
        SurfacePacer* pacer = nullptr;
        try {
          pacer = &SurfacePacer::from(m_surface);
        } catch (const std::bad_alloc&) {
          return false;
        }
        if (!wlr_surface_synced_init(&m_synced, m_surface, &kSyncedImpl, &m_pending, &m_current)) {
          return false;
        }
        // Retained while the fifo lives, so a hidden surface keeps ticking and its queue keeps draining.
        m_pacer = pacer;
        m_pacer->retain();
        m_pacerRefresh.notify = onPacerRefresh;
        wl_signal_add(&m_pacer->events.refresh, &m_pacerRefresh);
        m_pacerDestroy.notify = onPacerDestroy;
        wl_signal_add(&m_pacer->events.destroy, &m_pacerDestroy);

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

      static void handleDestroy(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

      // The resource is gone, by request or with its client: every held commit is released, as the protocol leaves
      // earlier content updates unaffected by the fifo's destruction.
      static void handleResourceDestroy(wl_resource* resource) {
        Fifo* fifo = fromResource(resource);
        wlr_surface* surface = fifo->m_surface;
        if (surface != nullptr) {
          const std::vector<std::uint32_t> released = fifo->m_queue.drain();
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

      static void onPacerRefresh(wl_listener* listener, void* /*data*/) {
        Fifo* fifo = nullptr;
        fifo = wl_container_of(listener, fifo, m_pacerRefresh);
        fifo->handlePacerRefresh();
      }

      static void onPacerDestroy(wl_listener* listener, void* /*data*/) {
        Fifo* fifo = nullptr;
        fifo = wl_container_of(listener, fifo, m_pacerDestroy);
        fifo->handlePacerDestroy();
      }

      // A commit that waits for the barrier while one is set, or while older commits are still held, is held too.
      void handleClientCommit() {
        if (!m_queue.shouldHold(m_pending.waitBarrier, isSynchronizedSubsurface(m_surface))) {
          return;
        }
        const std::uint32_t seq = wlr_surface_lock_pending(m_surface);
        m_queue.hold(HeldCommit{.seq = seq, .setsBarrier = m_pending.setBarrier});
        // Makes sure a refresh comes even when nothing else would draw the pacing output.
        if (m_pacer != nullptr) {
          m_pacer->requestFrame();
        }
      }

      void handleCommit() { m_queue.applied(m_current.setBarrier); }

      // A refresh clears the barrier and releases held commits up to the next one that sets it again. The pacing
      // output changing does not flush the queue: it simply follows the new output's refreshes.
      void handlePacerRefresh() {
        const std::vector<std::uint32_t> released = m_queue.refresh();
        // Unlocking can apply a commit synchronously, which re-enters handleCommit and re-arms the barrier.
        for (const std::uint32_t seq : released) {
          wlr_surface_unlock_cached(m_surface, seq);
        }
        if (!m_queue.empty() && m_pacer != nullptr) {
          m_pacer->requestFrame();
        }
      }

      // The pacer normally outlives this addon; if it goes first, stop listening and never touch it again.
      void handlePacerDestroy() {
        detachPacer();
        m_pacer = nullptr;
      }

      // The surface is being destroyed: its cached states, held commits included, go with it, so the queue is
      // dropped without unlocking anything.
      void handleSurfaceDestroy() {
        m_queue = FifoQueue{};
        detach();
      }

      void detachPacer() {
        wl_list_remove(&m_pacerRefresh.link);
        wl_list_init(&m_pacerRefresh.link);
        wl_list_remove(&m_pacerDestroy.link);
        wl_list_init(&m_pacerDestroy.link);
      }

      // Unhooks the fifo from its surface, leaving it inert.
      void detach() {
        if (m_pacer != nullptr) {
          detachPacer();
          m_pacer->release();
          m_pacer = nullptr;
        }
        wl_list_remove(&m_clientCommit.link);
        wl_list_remove(&m_commit.link);
        wlr_addon_finish(&m_addon);
        wlr_surface_synced_finish(&m_synced);
        m_surface = nullptr;
      }

      void postSurfaceDestroyed() {
        wl_resource_post_error(
            m_resource, WP_FIFO_V1_ERROR_SURFACE_DESTROYED, "the wp_fifo_v1's surface no longer exists"
        );
      }

      wl_resource* m_resource = nullptr;
      wlr_surface* m_surface = nullptr;
      SurfacePacer* m_pacer = nullptr;
      FifoQueue m_queue;
      wlr_addon m_addon{};
      wlr_surface_synced m_synced{};
      FifoState m_pending;
      FifoState m_current;
      wl_listener m_clientCommit{};
      wl_listener m_commit{};
      wl_listener m_pacerRefresh{};
      wl_listener m_pacerDestroy{};
    };

    const struct wp_fifo_v1_interface Fifo::kImplementation = {
        .set_barrier = Fifo::handleSetBarrier,
        .wait_barrier = Fifo::handleWaitBarrier,
        .destroy = Fifo::handleDestroy,
    };

  } // namespace

  struct FifoManager::Impl {
    wl_global* global = nullptr;

    static const struct wp_fifo_manager_v1_interface kImplementation;

    static void handleBind(wl_client* client, void* /*data*/, uint32_t version, uint32_t id) {
      wl_resource* resource =
          wl_resource_create(client, &wp_fifo_manager_v1_interface, std::min(version, kProtocolVersion), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      wl_resource_set_implementation(resource, &kImplementation, nullptr, nullptr);
    }

    static void handleDestroy(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

    static void handleGetFifo(wl_client* client, wl_resource* manager, uint32_t id, wl_resource* surfaceResource) {
      wlr_surface* surface = wlr_surface_from_resource(surfaceResource);
      if (Fifo::surfaceHasFifo(surface)) {
        wl_resource_post_error(
            manager, WP_FIFO_MANAGER_V1_ERROR_ALREADY_EXISTS, "the surface already has a wp_fifo_v1 object"
        );
        return;
      }
      wl_resource* resource = wl_resource_create(client, &wp_fifo_v1_interface, wl_resource_get_version(manager), id);
      if (resource == nullptr) {
        wl_client_post_no_memory(client);
        return;
      }
      auto* fifo = new (std::nothrow) Fifo(resource, surface);
      if (fifo == nullptr || !fifo->attach()) {
        delete fifo;
        wl_resource_destroy(resource);
        wl_client_post_no_memory(client);
        return;
      }
      // Owned by the resource; freed by Fifo::handleResourceDestroy.
      wl_resource_set_implementation(resource, &Fifo::kImplementation, fifo, Fifo::handleResourceDestroy);
    }
  };

  const struct wp_fifo_manager_v1_interface FifoManager::Impl::kImplementation = {
      .destroy = FifoManager::Impl::handleDestroy,
      .get_fifo = FifoManager::Impl::handleGetFifo,
  };

  FifoManager::FifoManager(wl_display* display) : m_impl(std::make_unique<Impl>()) {
    m_impl->global =
        wl_global_create(display, &wp_fifo_manager_v1_interface, kProtocolVersion, nullptr, Impl::handleBind);
  }

  FifoManager::~FifoManager() {
    if (m_impl->global != nullptr) {
      wl_global_destroy(m_impl->global);
    }
  }

  bool FifoManager::valid() const { return m_impl->global != nullptr; }

} // namespace umbriel
