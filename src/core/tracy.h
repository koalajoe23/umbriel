#pragma once

// Zones for the compositor's per-frame work, alongside umbrielfx's own zones
// for the render pass, and plots for per-frame values. No-ops without
// -Dtracy=enabled; a plot's value expression is then not evaluated.
#ifdef TRACY_ENABLE
#include <tracy/Tracy.hpp>
#define UMBRIEL_ZONE(name) ZoneScopedN(name)
#define UMBRIEL_PLOT(name, value) TracyPlot(name, value)
#else
#define UMBRIEL_ZONE(name)
#define UMBRIEL_PLOT(name, value) static_cast<void>(sizeof(value))
#endif
