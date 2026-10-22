#include "view/snap.h"

#include <algorithm>
#include <array>
#include <utility>

extern "C" {
#include <wlr/util/edges.h>
}

namespace umbriel {
  namespace {
    constexpr std::array kNames{"left",     "right",     "top",         "bottom",
                                "top_left", "top_right", "bottom_left", "bottom_right"};
  }

  std::string_view snapRegionName(SnapRegion region) { return kNames[static_cast<size_t>(region)]; }

  std::optional<SnapRegion> snapRegionNamed(std::string_view name) {
    for (size_t i = 0; i < kNames.size(); ++i) {
      if (name == kNames[i]) {
        return static_cast<SnapRegion>(i);
      }
    }
    return std::nullopt;
  }

  wlr_box snapRegionBox(const wlr_box& area, SnapRegion region, int spacing) {
    const bool right =
        region == SnapRegion::Right || region == SnapRegion::TopRight || region == SnapRegion::BottomRight;
    const bool splitX = region != SnapRegion::Top && region != SnapRegion::Bottom;
    const bool splitY = region != SnapRegion::Left && region != SnapRegion::Right;
    const bool bottom =
        region == SnapRegion::Bottom || region == SnapRegion::BottomLeft || region == SnapRegion::BottomRight;
    const int gapX = std::clamp(spacing, 0, std::max(0, area.width));
    const int gapY = std::clamp(spacing, 0, std::max(0, area.height));
    const int leftWidth = std::max(0, area.width - gapX) / 2;
    const int topHeight = std::max(0, area.height - gapY) / 2;
    return {
        .x = area.x + (right ? leftWidth + gapX : 0),
        .y = area.y + (bottom ? topHeight + gapY : 0),
        .width = splitX ? (right ? std::max(0, area.width - gapX - leftWidth) : leftWidth) : area.width,
        .height = splitY ? (bottom ? std::max(0, area.height - gapY - topHeight) : topHeight) : area.height,
    };
  }

  std::optional<SnapRegion> snapZoneAt(const wlr_box& output, double x, double y, uint32_t closedEdges) {
    if (output.width <= 0
        || output.height <= 0
        || x < output.x
        || y < output.y
        || x >= output.x + output.width
        || y >= output.y + output.height) {
      return std::nullopt;
    }
    const double left = x - output.x;
    const double right = output.x + output.width - x;
    const double top = y - output.y;
    const double bottom = output.y + output.height - y;
    const bool onLeft = left < kSnapEdge && (closedEdges & WLR_EDGE_LEFT) == 0;
    const bool onRight = right <= kSnapEdge && (closedEdges & WLR_EDGE_RIGHT) == 0;
    const bool onTop = top < kSnapEdge && (closedEdges & WLR_EDGE_TOP) == 0;
    const bool onBottom = bottom <= kSnapEdge && (closedEdges & WLR_EDGE_BOTTOM) == 0;
    if ((onLeft && top < kSnapCornerSpan) || (onTop && left < kSnapCornerSpan))
      return SnapRegion::TopLeft;
    if ((onRight && top < kSnapCornerSpan) || (onTop && right <= kSnapCornerSpan))
      return SnapRegion::TopRight;
    if ((onLeft && bottom <= kSnapCornerSpan) || (onBottom && left < kSnapCornerSpan))
      return SnapRegion::BottomLeft;
    if ((onRight && bottom <= kSnapCornerSpan) || (onBottom && right <= kSnapCornerSpan))
      return SnapRegion::BottomRight;
    if (onTop)
      return SnapRegion::Top;
    if (onBottom)
      return SnapRegion::Bottom;
    if (onLeft)
      return SnapRegion::Left;
    if (onRight)
      return SnapRegion::Right;
    return std::nullopt;
  }
} // namespace umbriel
