#include "input/shortcuts_inhibit.h"

#include "check.h"
#include "wlr.h"

namespace {

  struct TestSurface {
    TestSurface() { wl_signal_init(&surface.events.destroy); }

    wlr_surface surface{};
  };

} // namespace

UMBRIEL_TEST(manualInhibitionIsIndependentPerSurface) {
  TestSurface first;
  TestSurface second;
  umbriel::ManualShortcutsInhibit inhibition;

  CHECK(!inhibition.enabled(&first.surface));
  inhibition.enable(&first.surface);
  inhibition.enable(&second.surface);
  CHECK(inhibition.enabled(&first.surface));
  CHECK(inhibition.enabled(&second.surface));

  inhibition.disable(&first.surface);
  CHECK(!inhibition.enabled(&first.surface));
  CHECK(inhibition.enabled(&second.surface));
}

UMBRIEL_TEST(surfaceDestructionClearsManualInhibition) {
  TestSurface first;
  TestSurface second;
  umbriel::ManualShortcutsInhibit inhibition;
  inhibition.enable(&first.surface);
  inhibition.enable(&second.surface);
  CHECK(inhibition.enabled(&first.surface));

  wl_signal_emit_mutable(&first.surface.events.destroy, &first.surface);

  CHECK(!inhibition.enabled(&first.surface));
  CHECK(inhibition.enabled(&second.surface));
  CHECK(wl_list_empty(&first.surface.events.destroy.listener_list));
}

UMBRIEL_TEST(manualInhibitionUnlinksListenersOnClearAndShutdown) {
  TestSurface surface;
  {
    umbriel::ManualShortcutsInhibit inhibition;
    inhibition.enable(&surface.surface);
    inhibition.enable(&surface.surface);
    CHECK_EQ(wl_list_length(&surface.surface.events.destroy.listener_list), 1);

    inhibition.clear();
    CHECK(!inhibition.enabled(&surface.surface));
    CHECK(wl_list_empty(&surface.surface.events.destroy.listener_list));

    inhibition.enable(&surface.surface);
    CHECK(!wl_list_empty(&surface.surface.events.destroy.listener_list));
  }
  CHECK(wl_list_empty(&surface.surface.events.destroy.listener_list));
}

int main() { return RUN_TESTS(); }
