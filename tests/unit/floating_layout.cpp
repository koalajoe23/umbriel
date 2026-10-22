#include "check.h"
#include "layout/floating.h"
#include "layout/scrolling.h"

extern "C" {
#include <wlr/util/box.h>
}

using namespace umbriel;

UMBRIEL_TEST(insertedViewsAreNeverMembers) {
  auto layout = createLayout(LayoutMode::Floating);
  View* view = reinterpret_cast<View*>(uintptr_t{0x1000});
  layout->insertView(view, 0);
  layout->insertViewIntoColumn(view, 0, 0);
  layout->arrange({0, 0, 1280, 720});
  CHECK(layout->mode() == LayoutMode::Floating);
  CHECK_EQ(layout->columnOf(view), -1);
  CHECK_EQ(layout->rowOf(view), -1);
  CHECK(layout->columns().empty());
  CHECK_EQ(layout->targetBox(view).width, 0);
}

UMBRIEL_TEST(captureRestoresOnlyFloatingSnapshots) {
  FloatingLayout layout;
  const auto capture = layout.captureState();
  CHECK(capture.snapshot != nullptr);
  CHECK(capture.snapshot->mode() == LayoutMode::Floating);
  CHECK_EQ(capture.snapshot->memberCount(), size_t{0});
  CHECK(capture.members.empty());
  CHECK(layout.restoreState(*capture.snapshot, {}));
  const auto scrolling = ScrollingLayout{}.captureState();
  CHECK(!layout.restoreState(*scrolling.snapshot, {}));
  const LayoutMember member{};
  CHECK(!layout.restoreState(*capture.snapshot, std::span{&member, size_t{1}}));
}

int main() { return RUN_TESTS(); }
