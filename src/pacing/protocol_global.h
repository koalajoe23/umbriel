#pragma once

#include <cstdint>

struct wl_client;
struct wl_display;
struct wl_global;
struct wl_interface;
struct wl_resource;

namespace umbriel {

  // What a pacing protocol's manager global binds: its interface, the highest version served, and the manager
  // requests' implementation. The spec must outlive the global (a static constant).
  struct ProtocolGlobalSpec {
    const wl_interface* interface;
    uint32_t version;
    const void* implementation;
  };

  // A manager global whose bound resources carry no state of their own: each bind creates a resource at the lower of
  // the client's and the spec's version, with the spec's implementation. Destroyed with the object.
  class ProtocolGlobal {
  public:
    ProtocolGlobal(wl_display* display, const ProtocolGlobalSpec& spec);
    ~ProtocolGlobal();

    ProtocolGlobal(const ProtocolGlobal&) = delete;
    ProtocolGlobal& operator=(const ProtocolGlobal&) = delete;

    [[nodiscard]] bool valid() const { return m_global != nullptr; }

  private:
    static void handleBind(wl_client* client, void* data, uint32_t version, uint32_t id);

    wl_global* m_global = nullptr;
  };

  // The destructor request of a protocol object whose resource-destroy handler does the cleanup.
  void handleDestroyRequest(wl_client* client, wl_resource* resource);

} // namespace umbriel
