#include "input/shortcuts_inhibit.h"

#include "wlr.h"

namespace umbriel {

  ManualShortcutsInhibit::~ManualShortcutsInhibit() { clear(); }

  bool ManualShortcutsInhibit::enabled(wlr_surface* surface) const { return m_entries.contains(surface); }

  void ManualShortcutsInhibit::enable(wlr_surface* surface) {
    if (surface == nullptr || enabled(surface)) {
      return;
    }
    auto entry = std::make_unique<Entry>();
    entry->owner = this;
    entry->surface = surface;
    entry->destroy.notify = onSurfaceDestroy;
    Entry* watch = entry.get();
    m_entries.emplace(surface, std::move(entry));
    wl_signal_add(&surface->events.destroy, &watch->destroy);
  }

  void ManualShortcutsInhibit::disable(wlr_surface* surface) {
    const auto found = m_entries.find(surface);
    if (found != m_entries.end()) {
      wl_list_remove(&found->second->destroy.link);
      m_entries.erase(found);
    }
  }

  void ManualShortcutsInhibit::clear() {
    while (!m_entries.empty()) {
      disable(m_entries.begin()->first);
    }
  }

  void ManualShortcutsInhibit::onSurfaceDestroy(wl_listener* listener, void* /*data*/) {
    Entry* entry = nullptr;
    entry = wl_container_of(listener, entry, destroy);
    entry->owner->disable(entry->surface);
  }

} // namespace umbriel
