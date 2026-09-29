#include "pacing/pacing.h"

#include <wayland-server-core.h>

namespace umbriel {

  struct Pacing::Impl {
    wl_display* display = nullptr;
  };

  Pacing::Pacing(wl_display* display) : m_impl(std::make_unique<Impl>()) { m_impl->display = display; }

  Pacing::~Pacing() = default;

  bool Pacing::valid() const { return m_impl->display != nullptr; }

} // namespace umbriel
