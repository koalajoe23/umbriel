#include "input/system_keymap.h"

#if UMBRIEL_HAS_SD_BUS
#include "core/log.h"

#include <cstdlib>
#include <memory>
#include <systemd/sd-bus.h>
#endif

namespace umbriel {

#if UMBRIEL_HAS_SD_BUS
  namespace {
    constexpr Logger kLog("keyboard");

    // localed is bus-activated, so the first call may wait for it to start. The compositor blocks meanwhile.
    constexpr uint64_t kLocaledTimeoutUsec = 2'000'000;

    struct BusDeleter {
      void operator()(sd_bus* bus) const { sd_bus_flush_close_unref(bus); }
    };

    // Missing properties read as empty, as localed reports for an unset field.
    bool readProperty(sd_bus* bus, const char* name, std::string& value) {
      sd_bus_error error = SD_BUS_ERROR_NULL;
      char* text = nullptr;
      const int result = sd_bus_get_property_string(
          bus, "org.freedesktop.locale1", "/org/freedesktop/locale1", "org.freedesktop.locale1", name, &error, &text
      );
      if (result < 0) {
        kLog.debug("systemd-localed {} unavailable: {}", name, error.message != nullptr ? error.message : "no reply");
        sd_bus_error_free(&error);
        return false;
      }
      value = text != nullptr ? text : "";
      free(text); // NOLINT(cppcoreguidelines-no-malloc): sd-bus hands out a malloc'd string.
      return true;
    }

    bool compiles(const XkbNames& names) {
      xkb_context* context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
      if (context == nullptr) {
        return false;
      }
      const xkb_rule_names rules{
          .rules = nullptr,
          .model = names.model.empty() ? nullptr : names.model.c_str(),
          .layout = names.layout.empty() ? nullptr : names.layout.c_str(),
          .variant = names.variant.empty() ? nullptr : names.variant.c_str(),
          .options = names.options.empty() ? nullptr : names.options.c_str(),
      };
      xkb_keymap* keymap = xkb_keymap_new_from_names(context, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
      const bool valid = keymap != nullptr;
      xkb_keymap_unref(keymap);
      xkb_context_unref(context);
      return valid;
    }
  } // namespace
#endif

  XkbNames querySystemKeymap() {
#if UMBRIEL_HAS_SD_BUS
    sd_bus* rawBus = nullptr;
    if (sd_bus_open_system(&rawBus) < 0) {
      kLog.debug("system bus unavailable; using the libxkbcommon default keymap");
      return {};
    }
    const std::unique_ptr<sd_bus, BusDeleter> bus(rawBus);
    sd_bus_set_method_call_timeout(bus.get(), kLocaledTimeoutUsec);
    XkbNames names;
    if (!readProperty(bus.get(), "X11Layout", names.layout)
        || !readProperty(bus.get(), "X11Variant", names.variant)
        || !readProperty(bus.get(), "X11Options", names.options)
        || !readProperty(bus.get(), "X11Model", names.model)) {
      return {};
    }
    if (names.empty()) {
      return {};
    }
    if (!compiles(names)) {
      kLog.warn(
          "ignoring systemd-localed keymap layout='{}' variant='{}' options='{}' model='{}' (invalid XKB "
          "configuration)",
          names.layout, names.variant, names.options, names.model
      );
      return {};
    }
    return names;
#else
    return {};
#endif
  }

} // namespace umbriel
