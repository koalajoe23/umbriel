#include "pacing/protocol_global.h"

#include <algorithm>
#include <wayland-server-core.h>

namespace umbriel {

  ProtocolGlobal::ProtocolGlobal(wl_display* display, const ProtocolGlobalSpec& spec)
      : m_global(wl_global_create(
            display, spec.interface, static_cast<int>(spec.version), const_cast<ProtocolGlobalSpec*>(&spec), handleBind
        )) {}

  ProtocolGlobal::~ProtocolGlobal() {
    if (m_global != nullptr) {
      wl_global_destroy(m_global);
    }
  }

  void ProtocolGlobal::handleBind(wl_client* client, void* data, uint32_t version, uint32_t id) {
    const auto* spec = static_cast<const ProtocolGlobalSpec*>(data);
    wl_resource* resource =
        wl_resource_create(client, spec->interface, static_cast<int>(std::min(version, spec->version)), id);
    if (resource == nullptr) {
      wl_client_post_no_memory(client);
      return;
    }
    wl_resource_set_implementation(resource, spec->implementation, nullptr, nullptr);
  }

  void handleDestroyRequest(wl_client* /*client*/, wl_resource* resource) { wl_resource_destroy(resource); }

} // namespace umbriel
