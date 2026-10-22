#include "config/resolve.h"
#include "input/cursor.h"
#include "server/server.h"
#include "view/size_hints.h"
#include "view/view.h"
#include "view/view_internal.h"
#include "workspace/scratchpad.h"
#include "workspace/workspace.h"

namespace umbriel {
  bool View::canSnap() const {
    const ScratchpadManager* scratchpad = m_server->scratchpadManager();
    return m_mapped
        && !m_tiled
        && !attached()
        && !m_pinned
        && !scheduledFullscreen()
        && !currentFullscreen()
        && (scratchpad == nullptr || !scratchpad->contains(this));
  }

  std::optional<wlr_box>
  View::floatingSnappedBox(SnapRegion region, const wlr_box& usable, const ResolvedLayoutConfig* layout) const {
    const auto resolved = layout != nullptr ? *layout
        : m_workspace != nullptr            ? m_workspace->layoutConfig()
                                            : resolveGlobalLayout(config());
    const wlr_box tile = snapRegionBox(floatingMarginArea(usable, &resolved), region, resolved.totalGap);
    // Snapping leaves maximize-to-edges, which makes the title bar visible again.
    const int bar = wantsTitleBar(m_titleBarMode.value_or(config().appearance.titleBar.mode))
        ? config().appearance.titleBar.height
        : 0;
    if (tile.width <= 0 || tile.height <= bar) {
      return std::nullopt;
    }
    const wlr_box content = contentBoxInTile(tile, bar);
    const SizeHints hints = sizeHints();
    if (hints.minWidth > content.width || hints.minHeight > content.height) {
      return std::nullopt;
    }
    const int width = clampWidth(content.width, hints);
    const int height = clampHeight(content.height, hints);
    return wlr_box{
        .x = content.x + (content.width - width) / 2,
        .y = content.y + (content.height - height) / 2,
        .width = width,
        .height = height,
    };
  }

  bool View::snapTo(SnapRegion region, std::optional<FloatingPoint> restoreOrigin) {
    if (!canSnap()) {
      return false;
    }
    if (m_snapRegion == region) {
      unsnap();
      return true;
    }
    const auto box = floatingSnappedBox(region, floatingUsableArea());
    if (!box) {
      return false;
    }
    if (!m_snapRegion) {
      const auto [width, height] = floatingSize();
      m_snapRestoreBox = m_hasMaximizeRestoreBox
          ? m_maximizeRestoreBox
          : wlr_box{.x = layoutTargetX(), .y = layoutTargetY(), .width = width, .height = height};
      if (restoreOrigin) {
        m_snapRestoreBox.x = restoreOrigin->x;
        m_snapRestoreBox.y = restoreOrigin->y;
      }
      m_hasSnapRestoreBox = width > 0 && height > 0;
    }
    dropMaximizedForResize();
    m_snapRegion = region;
    m_snappedBox = *box;
    requestFloatingSize(box->width, box->height);
    beginResizeAnimation(box->width, box->height);
    animateTo(box->x, box->y);
    syncSsdFrame();
    m_server->scheduleIpcWindowsEvent();
    return true;
  }

  void View::clearSnapState() {
    if (m_snapRegion) {
      m_server->scheduleIpcWindowsEvent();
    }
    m_snapRegion.reset();
    m_hasSnapRestoreBox = false;
    m_snappedBox = {};
  }

  void View::unsnap(bool animate) {
    if (!m_snapRegion || !canSnap()) {
      return;
    }
    const auto restore = m_hasSnapRestoreBox ? std::optional{m_snapRestoreBox} : std::nullopt;
    clearSnapState();
    dropMaximizedForResize();
    if (!restore) {
      return;
    }
    requestFloatingSize(restore->width, restore->height);
    if (animate) {
      beginResizeAnimation(restore->width, restore->height);
      animateTo(restore->x, restore->y);
    } else {
      cancelSizeAnimation();
      setPosition(restore->x, restore->y);
    }
    rememberFloatingPosition();
  }

  void View::refitFloatingSnapped() {
    if (!m_snapRegion || !canSnap() || scheduledMaximized() || m_server->cursor()->isDraggingView(this)) {
      return;
    }
    const auto box = floatingSnappedBox(*m_snapRegion, floatingUsableArea());
    if (!box) {
      unsnap();
      return;
    }
    if (box->x == m_snappedBox.x
        && box->y == m_snappedBox.y
        && box->width == m_snappedBox.width
        && box->height == m_snappedBox.height) {
      return;
    }
    m_snappedBox = *box;
    requestFloatingSize(box->width, box->height);
    setPosition(box->x, box->y);
    syncFloatingSurfaceClip();
  }
} // namespace umbriel
