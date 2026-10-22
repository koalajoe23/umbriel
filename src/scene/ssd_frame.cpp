#include "scene/ssd_frame.h"

#include "config/config.h"
#include "scene/color.h"
#include "scene/tab_label_cache.h"
#include "view/border_ring.h"
#include "wlr.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace umbriel {
  namespace {
    bool rejectInput(wlr_scene_buffer*, double*, double*) { return false; }
  } // namespace

  SsdFrame::SsdFrame(wlr_scene_tree* parent, std::shared_ptr<TabLabelCache> labels)
      : m_tree(wlr_scene_tree_create(parent)), m_labels(std::move(labels)) {
    const float clear[4]{};
    m_background = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_close = wlr_scene_rect_create(m_tree, 1, 1, clear);
    m_maximize = wlr_scene_rect_create(m_tree, 1, 1, clear);
    for (auto*& grip : m_grips)
      grip = wlr_scene_rect_create(m_tree, 1, 1, clear);
    wlr_scene_node_set_enabled(&m_tree->node, false);
  }

  SsdFrame::~SsdFrame() { wlr_scene_node_destroy(&m_tree->node); }

  void SsdFrame::layout(const ViewChromeGeometry& geometry) {
    if (m_geometry == geometry)
      return;
    m_geometry = geometry;
    redraw();
  }
  void SsdFrame::setFocused(bool focused) {
    if (m_focused == focused)
      return;
    m_focused = focused;
    redraw();
  }
  void SsdFrame::setAlpha(float alpha) {
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    if (m_alpha == alpha)
      return;
    m_alpha = alpha;
    updateColors();
  }
  void SsdFrame::reloadConfig() { redraw(); }
  void SsdFrame::setTitle(std::string_view title) {
    if (m_title == title)
      return;
    m_title = title;
    redraw();
  }
  void SsdFrame::setResizable(bool resizable) {
    if (m_resizable == resizable)
      return;
    m_resizable = resizable;
    redraw();
  }
  void SsdFrame::setMaximizeEnabled(bool enabled) {
    if (m_maximizeEnabled == enabled)
      return;
    m_maximizeEnabled = enabled;
    updateColors();
  }
  void SsdFrame::setMaximized(bool maximized) { m_maximized = maximized; }
  void SsdFrame::setHovered(SsdFramePart part) {
    if (m_hovered == part)
      return;
    m_hovered = part;
    updateColors();
  }
  void SsdFrame::setPressed(SsdFramePart part) {
    if (m_pressed == part)
      return;
    m_pressed = part;
    updateColors();
  }

  int SsdFrame::zoomed(int value) const { return static_cast<int>(std::lround(value * m_geometry.zoom)); }

  void SsdFrame::updateColors() {
    const auto& colors = config().colors.titleBar;
    float color[4];
    premultiplied(color, m_focused ? colors.background : colors.backgroundUnfocused, m_alpha);
    wlr_scene_rect_set_color(m_background, color);
    const auto button = [&](wlr_scene_rect* rect, const std::array<float, 4>& base, SsdFramePart part, bool enabled) {
      auto value = base;
      const float alpha = !enabled ? 0.35F : (m_hovered == part ? 1.0F : 0.85F);
      if (enabled && m_pressed == part) {
        for (size_t i = 0; i < 3; ++i)
          value[i] *= 0.8F;
      }
      premultiplied(color, value, m_alpha * alpha);
      wlr_scene_rect_set_color(rect, color);
    };
    button(m_close, colors.close, SsdFramePart::Close, true);
    button(m_maximize, colors.maximize, SsdFramePart::Maximize, m_maximizeEnabled);
    if (m_label != nullptr)
      wlr_scene_buffer_set_opacity(m_label, m_alpha);
  }

  void SsdFrame::redraw() {
    const auto& g = m_geometry;
    const int h = g.titleBarHeight;
    const bool shown = !g.suppressed && h > 0 && g.contentWidth > 0 && g.contentHeight > 0;
    wlr_scene_node_set_enabled(&m_tree->node, shown);
    if (!shown)
      return;
    wlr_scene_node_set_position(&m_tree->node, zoomed(g.contentX), zoomed(g.contentY));
    const auto place = [&](wlr_scene_rect* rect, const wlr_box& box) {
      wlr_scene_node_set_position(&rect->node, zoomed(box.x), zoomed(box.y));
      wlr_scene_rect_set_size(
          rect, std::max(1, zoomed(box.x + box.width) - zoomed(box.x)),
          std::max(1, zoomed(box.y + box.height) - zoomed(box.y))
      );
      rect->accepts_input = !m_preview;
    };
    place(m_background, {0, -h, g.contentWidth, h});
    const int radius = g.borderInset > 0 || config().appearance.cornerRadius > 0
        ? nestedRadius(config().appearance.cornerRadius, g.borderInset)
        : 0;
    wlr_scene_rect_set_corner_radii(m_background, corner_radii_new(zoomed(radius), zoomed(radius), 0, 0));
    const auto& style = config().appearance.titleBar;
    const TitleBarLayout parts = titleBarLayout(g.contentWidth, h, style.buttonPosition);
    const auto drawButton = [&](wlr_scene_rect* button, wlr_box box) {
      box.y -= h;
      place(button, box);
      wlr_scene_rect_set_corner_radius(button, zoomed(box.width) / 2);
      wlr_scene_node_set_enabled(&button->node, box.x >= 0 && box.x + box.width <= g.contentWidth);
    };
    drawButton(m_close, parts.close);
    drawButton(m_maximize, parts.maximize);
    const int band = g.borderInset + kSsdResizeBand;
    const std::array<wlr_box, 4> grips{{
        {-band, -h - band, g.contentWidth + 2 * band, band},
        {-band, g.contentHeight, g.contentWidth + 2 * band, band},
        {-band, -h, band, g.contentHeight + h},
        {g.contentWidth, -h, band, g.contentHeight + h},
    }};
    for (size_t i = 0; i < m_grips.size(); ++i) {
      place(m_grips[i], grips[i]);
      wlr_scene_node_set_enabled(&m_grips[i]->node, m_resizable && !m_preview);
    }
    const auto& colors = config().colors.titleBar;
    const TabLabel* label = m_labels->label({
        .text = m_title,
        .color = m_focused ? colors.text : colors.textUnfocused,
        .font = style.font,
        .maxWidth = parts.title.width,
        .scale = std::max(1.0, std::ceil(static_cast<double>(g.scale))),
    });
    if (label == nullptr) {
      if (m_label != nullptr) {
        wlr_scene_node_destroy(&m_label->node);
        m_label = nullptr;
      }
    } else {
      if (m_label == nullptr) {
        m_label = wlr_scene_buffer_create(m_tree, label->buffer);
        m_label->point_accepts_input = rejectInput;
        wlr_scene_node_place_above(&m_label->node, &m_background->node);
      } else {
        wlr_scene_buffer_set_buffer(m_label, label->buffer);
      }
      wlr_scene_buffer_set_dest_size(m_label, std::max(1, zoomed(label->width)), std::max(1, zoomed(label->height)));
      wlr_scene_node_set_position(&m_label->node, zoomed(parts.title.x), zoomed(-h + (h - label->height) / 2));
    }
    updateColors();
  }

  bool SsdFrame::contains(const wlr_scene_node* node) const {
    for (const wlr_scene_node* current = node; current != nullptr;
         current = current->parent != nullptr ? &current->parent->node : nullptr) {
      if (current == &m_tree->node)
        return true;
    }
    return false;
  }
  SsdFramePoint SsdFrame::pointAt(double x, double y) const {
    if (m_preview || m_geometry.suppressed || !m_tree->node.enabled)
      return {};
    return ssdFramePointAt(
        m_geometry.contentWidth, m_geometry.contentHeight, m_geometry.titleBarHeight, m_geometry.borderInset,
        m_resizable, config().appearance.titleBar.buttonPosition, x, y
    );
  }

  void SsdFrame::snapshot(wlr_scene_tree* parent) const {
    if (!m_tree->node.enabled)
      return;
    auto* tree = parent;
    for (const auto* source : {m_background, m_close, m_maximize}) {
      if (!source->node.enabled)
        continue;
      auto* copy = wlr_scene_rect_create(tree, source->width, source->height, source->color);
      copy->accepts_input = false;
      wlr_scene_rect_set_corner_radii(copy, source->corners);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + source->node.x, m_tree->node.y + source->node.y);
    }
    if (m_label != nullptr && m_label->buffer != nullptr) {
      auto* copy = wlr_scene_buffer_create(tree, m_label->buffer);
      copy->point_accepts_input = rejectInput;
      wlr_scene_buffer_set_dest_size(copy, m_label->dst_width, m_label->dst_height);
      wlr_scene_buffer_set_opacity(copy, m_label->opacity);
      wlr_scene_node_set_position(&copy->node, m_tree->node.x + m_label->node.x, m_tree->node.y + m_label->node.y);
    }
  }
  std::unique_ptr<ViewChromeAttachment> SsdFrame::makePreview(wlr_scene_tree* parent) const {
    auto preview = std::make_unique<SsdFrame>(parent, m_labels);
    preview->m_preview = true;
    syncPreview(*preview);
    return preview;
  }
  void SsdFrame::syncPreview(ViewChromeAttachment& preview) const {
    if (auto* frame = dynamic_cast<SsdFrame*>(&preview)) {
      frame->setTitle(m_title);
      frame->setResizable(false);
      frame->setMaximizeEnabled(m_maximizeEnabled);
      frame->setMaximized(m_maximized);
      frame->setFocused(m_focused);
      frame->reloadConfig();
    }
  }
} // namespace umbriel
