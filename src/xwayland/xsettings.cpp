#include "xwayland/xsettings.h"

#include <algorithm>
#include <cmath>

namespace umbriel {

  namespace {
    constexpr double kBaseDpi = 96.0 * 1024.0;
    constexpr uint8_t kLsbFirst = 0;
    constexpr uint8_t kTypeInteger = 0;
    constexpr uint8_t kTypeString = 1;

    void putCard16(std::vector<uint8_t>& out, uint16_t value) {
      out.push_back(static_cast<uint8_t>(value & 0xFFU));
      out.push_back(static_cast<uint8_t>(value >> 8U));
    }

    void putCard32(std::vector<uint8_t>& out, uint32_t value) {
      for (unsigned shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFU));
      }
    }

    void putPadded(std::vector<uint8_t>& out, std::string_view bytes) {
      out.insert(out.end(), bytes.begin(), bytes.end());
      out.resize(out.size() + ((4 - (bytes.size() % 4)) % 4), 0);
    }

    void putHeader(std::vector<uint8_t>& out, uint8_t type, std::string_view name, uint32_t serial) {
      out.push_back(type);
      out.push_back(0);
      putCard16(out, static_cast<uint16_t>(name.size()));
      putPadded(out, name);
      putCard32(out, serial);
    }

    void putInteger(std::vector<uint8_t>& out, std::string_view name, int32_t value, uint32_t serial) {
      putHeader(out, kTypeInteger, name, serial);
      putCard32(out, static_cast<uint32_t>(value));
    }

    void putString(std::vector<uint8_t>& out, std::string_view name, std::string_view value, uint32_t serial) {
      putHeader(out, kTypeString, name, serial);
      putCard32(out, static_cast<uint32_t>(value.size()));
      putPadded(out, value);
    }
  } // namespace

  XsettingsValues xsettingsFor(double scale, std::string_view cursorTheme, int cursorSize) {
    const double effective = std::max(scale, 1.0);
    const auto integer = static_cast<int32_t>(std::floor(effective));
    const auto xftDpi = static_cast<int32_t>(std::lround(kBaseDpi * effective));
    return {
        .windowScalingFactor = integer,
        .unscaledDpi = static_cast<int32_t>(std::lround(static_cast<double>(xftDpi) / integer)),
        .xftDpi = xftDpi,
        .cursorTheme = std::string(cursorTheme),
        .cursorSize = static_cast<int32_t>(std::lround(cursorSize * effective)),
    };
  }

  std::vector<uint8_t> encodeXsettings(uint32_t serial, const XsettingsValues& values) {
    std::vector<uint8_t> out;
    out.push_back(kLsbFirst);
    out.resize(4, 0);
    putCard32(out, serial);
    putCard32(out, values.cursorTheme.empty() ? 4 : 5);
    putInteger(out, "Gdk/WindowScalingFactor", values.windowScalingFactor, serial);
    putInteger(out, "Gdk/UnscaledDPI", values.unscaledDpi, serial);
    putInteger(out, "Xft/DPI", values.xftDpi, serial);
    putInteger(out, "Gtk/CursorThemeSize", values.cursorSize, serial);
    if (!values.cursorTheme.empty()) {
      putString(out, "Gtk/CursorThemeName", values.cursorTheme, serial);
    }
    return out;
  }

} // namespace umbriel
