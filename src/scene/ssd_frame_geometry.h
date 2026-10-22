#pragma once

#include <cstdint>

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  enum class TitleBarButtonSide : uint8_t { Left, Right };
  enum class SsdFramePart : uint8_t { None, TitleBar, Close, Maximize, Resize };

  constexpr int kTitleBarButtonGap = 8;
  constexpr int kTitleBarTitlePadding = 8;
  constexpr int kSsdResizeBand = 6;
  constexpr int kSsdResizeCorner = 20;

  struct TitleBarLayout {
    wlr_box close{};
    wlr_box maximize{};
    wlr_box title{};
  };
  struct SsdFramePoint {
    SsdFramePart part = SsdFramePart::None;
    uint32_t edges = 0;
  };

  [[nodiscard]] TitleBarLayout titleBarLayout(int width, int height, TitleBarButtonSide side);
  // Coordinates relative to client content; the title bar lies above y = 0.
  [[nodiscard]] SsdFramePoint ssdFramePointAt(
      int contentWidth, int contentHeight, int titleBarHeight, int borderInset, bool resizable, TitleBarButtonSide side,
      double x, double y
  );
  [[nodiscard]] wlr_box contentBoxInTile(const wlr_box& tile, int titleBarHeight);
} // namespace umbriel
