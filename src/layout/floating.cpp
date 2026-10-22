#include "layout/floating.h"

extern "C" {
#include <wlr/util/box.h>
}

namespace umbriel {
  namespace {
    class FloatingSnapshot final : public LayoutSnapshot {
    public:
      [[nodiscard]] LayoutMode mode() const override { return LayoutMode::Floating; }
      [[nodiscard]] size_t memberCount() const override { return 0; }
    };
  } // namespace

  LayoutCapture FloatingLayout::captureState() const { return {std::make_shared<FloatingSnapshot>(), {}}; }
  bool FloatingLayout::restoreState(const LayoutSnapshot& snapshot, std::span<const LayoutMember> members) {
    return snapshot.mode() == LayoutMode::Floating && members.empty();
  }
  wlr_box FloatingLayout::targetBox(const View*) const { return {}; }
  const std::vector<Column>& FloatingLayout::columns() const {
    static const std::vector<Column> empty;
    return empty;
  }
} // namespace umbriel
