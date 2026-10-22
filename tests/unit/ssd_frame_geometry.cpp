#include "scene/ssd_frame_geometry.h"

#include "check.h"

extern "C" {
#include <wlr/util/edges.h>
}

using namespace umbriel;

UMBRIEL_TEST(rightButtonsPutCloseOutermost) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Right);
  CHECK_EQ(parts.close.x, 459);
  CHECK_EQ(parts.close.y, 7);
  CHECK_EQ(parts.close.width, 14);
  CHECK_EQ(parts.close.height, 14);
  CHECK_EQ(parts.maximize.x, 437);
  CHECK_EQ(parts.title.x, 8);
  CHECK_EQ(parts.title.width, 421);
}

UMBRIEL_TEST(leftButtonsMirror) {
  const auto parts = titleBarLayout(480, 28, TitleBarButtonSide::Left);
  CHECK_EQ(parts.close.x, 7);
  CHECK_EQ(parts.maximize.x, 29);
  CHECK_EQ(parts.title.x, 51);
  CHECK_EQ(parts.title.width, 421);
}

UMBRIEL_TEST(buttonHitSpansTitleBarHeight) {
  for (const double y : {-28.0, -14.0, -0.1}) {
    CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 466, y).part, SsdFramePart::Close);
    CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 444, y).part, SsdFramePart::Maximize);
    CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Left, 14, y).part, SsdFramePart::Close);
  }
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 466, 0).part, SsdFramePart::None);
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 200, -15).part, SsdFramePart::TitleBar);
}

UMBRIEL_TEST(resizeBandOnlyWhenResizable) {
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, false, TitleBarButtonSide::Right, -3, 100).part, SsdFramePart::None);
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, -3, 100).edges, uint32_t{WLR_EDGE_LEFT});
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, -8.1, 100).part, SsdFramePart::None);
  CHECK_EQ(ssdFramePointAt(480, 300, 0, 2, true, TitleBarButtonSide::Right, -3, 100).part, SsdFramePart::None);
}

UMBRIEL_TEST(cornerExtensionAddsBothEdges) {
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, -3, -15).edges,
      uint32_t{WLR_EDGE_LEFT | WLR_EDGE_TOP}
  );
  CHECK_EQ(
      ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 470, 302).edges,
      uint32_t{WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM}
  );
  CHECK_EQ(ssdFramePointAt(480, 300, 28, 2, true, TitleBarButtonSide::Right, 200, -31).edges, uint32_t{WLR_EDGE_TOP});
}

UMBRIEL_TEST(contentBoxInTileRemovesTitleBar) {
  const auto content = contentBoxInTile({8, 8, 1264, 704}, 28);
  CHECK_EQ(content.x, 8);
  CHECK_EQ(content.y, 36);
  CHECK_EQ(content.width, 1264);
  CHECK_EQ(content.height, 676);
  CHECK_EQ(contentBoxInTile({-200, 40, 400, 12}, 28).height, 1);
  CHECK_EQ(contentBoxInTile({8, 8, 1264, 704}, 0).height, 704);
}

int main() { return RUN_TESTS(); }
