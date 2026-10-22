#include "scene/ssd_frame_geometry.h"

#include <algorithm>

extern "C" {
#include <wlr/util/edges.h>
}

namespace umbriel {
  TitleBarLayout titleBarLayout(int width, int height, TitleBarButtonSide side) {
    const int diameter = std::max(8, (height + 1) / 2);
    const int inset = (height - diameter) / 2;
    TitleBarLayout result{
        .close = {0, inset, diameter, diameter},
        .maximize = {0, inset, diameter, diameter},
        .title = {kTitleBarTitlePadding, 0, 0, height},
    };
    if (side == TitleBarButtonSide::Right) {
      result.close.x = width - inset - diameter;
      result.maximize.x = result.close.x - kTitleBarButtonGap - diameter;
      result.title.width = std::max(0, result.maximize.x - 2 * kTitleBarTitlePadding);
    } else {
      result.close.x = inset;
      result.maximize.x = inset + diameter + kTitleBarButtonGap;
      result.title.x = result.maximize.x + diameter + kTitleBarTitlePadding;
      result.title.width = std::max(0, width - result.title.x - kTitleBarTitlePadding);
    }
    return result;
  }

  SsdFramePoint
  ssdFramePointAt(int cw, int ch, int h, int b, bool resizable, TitleBarButtonSide side, double x, double y) {
    if (cw <= 0 || ch <= 0 || h <= 0)
      return {};
    if (x >= 0 && x < cw && y >= -h && y < 0) {
      const TitleBarLayout buttons = titleBarLayout(cw, h, side);
      const auto inButton = [x](const wlr_box& box) {
        return x >= box.x - kTitleBarButtonGap / 2 && x < box.x + box.width + kTitleBarButtonGap / 2;
      };
      if (inButton(buttons.close))
        return {SsdFramePart::Close, 0};
      if (inButton(buttons.maximize))
        return {SsdFramePart::Maximize, 0};
      return {SsdFramePart::TitleBar, 0};
    }
    if (!resizable
        || x < -b - kSsdResizeBand
        || x >= cw + b + kSsdResizeBand
        || y < -h - b - kSsdResizeBand
        || y >= ch + b + kSsdResizeBand)
      return {};
    uint32_t edges = 0;
    if (x < 0)
      edges |= WLR_EDGE_LEFT;
    if (x >= cw)
      edges |= WLR_EDGE_RIGHT;
    if (y < -h)
      edges |= WLR_EDGE_TOP;
    if (y >= ch)
      edges |= WLR_EDGE_BOTTOM;
    if (edges == 0)
      return {};
    if ((edges & (WLR_EDGE_LEFT | WLR_EDGE_RIGHT)) != 0) {
      if (y < -h - b + kSsdResizeCorner)
        edges |= WLR_EDGE_TOP;
      if (y >= ch + b - kSsdResizeCorner)
        edges |= WLR_EDGE_BOTTOM;
    }
    if ((edges & (WLR_EDGE_TOP | WLR_EDGE_BOTTOM)) != 0) {
      if (x < -b + kSsdResizeCorner)
        edges |= WLR_EDGE_LEFT;
      if (x >= cw + b - kSsdResizeCorner)
        edges |= WLR_EDGE_RIGHT;
    }
    return {SsdFramePart::Resize, edges};
  }

  wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight) {
    return {tile.x, tile.y + titleBarHeight, tile.width, std::max(1, tile.height - titleBarHeight)};
  }
} // namespace umbriel
