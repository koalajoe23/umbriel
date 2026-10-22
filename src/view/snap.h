#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class SnapRegion : uint8_t { Left, Right, Top, Bottom, TopLeft, TopRight, BottomLeft, BottomRight };
  constexpr int kSnapEdge = 16;
  constexpr int kSnapCornerSpan = 100;
  constexpr int kSnapDwellMs = 150;

  [[nodiscard]] std::string_view snapRegionName(SnapRegion region);
  [[nodiscard]] std::optional<SnapRegion> snapRegionNamed(std::string_view name);
  [[nodiscard]] wlr_box snapRegionBox(const wlr_box& area, SnapRegion region, int spacing);
  // Output coordinates and thresholds are logical. Shared edges are excluded until the drag dwell expires.
  [[nodiscard]] std::optional<SnapRegion> snapZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges);
} // namespace umbriel
