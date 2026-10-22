// What X11 clients read from the XSETTINGS manager: the scale split GTK, Chromium, and CEF expect, and a property
// they can parse under the XSETTINGS 0.5 wire format.
#include "check.h"
#include "xwayland/xsettings.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

  using umbriel::encodeXsettings;
  using umbriel::xsettingsFor;
  using Value = std::variant<int32_t, std::string>;

  struct Decoded {
    uint32_t serial = 0;
    std::map<std::string, Value> settings;
    std::map<std::string, uint32_t> serials;
  };

  // An independent little-endian reader of the specification's layout; nullopt for anything malformed.
  std::optional<Decoded> decode(const std::vector<uint8_t>& data) {
    size_t at = 0;
    const auto card32 = [&](uint32_t& out) {
      if (at + 4 > data.size()) {
        return false;
      }
      out = data[at] | (data[at + 1] << 8U) | (data[at + 2] << 16U) | (static_cast<uint32_t>(data[at + 3]) << 24U);
      at += 4;
      return true;
    };
    const auto padded = [&](size_t length, std::string& out) {
      const size_t span = length + ((4 - (length % 4)) % 4);
      if (at + span > data.size()) {
        return false;
      }
      out.assign(
          data.begin() + static_cast<std::ptrdiff_t>(at), data.begin() + static_cast<std::ptrdiff_t>(at + length)
      );
      for (size_t i = at + length; i < at + span; ++i) {
        if (data[i] != 0) {
          return false;
        }
      }
      at += span;
      return true;
    };
    if (data.size() < 12 || data[0] != 0 || (data.size() % 4) != 0) {
      return std::nullopt;
    }
    at = 4;
    Decoded decoded;
    uint32_t count = 0;
    if (!card32(decoded.serial) || !card32(count)) {
      return std::nullopt;
    }
    for (uint32_t i = 0; i < count; ++i) {
      if (at + 4 > data.size()) {
        return std::nullopt;
      }
      const uint8_t type = data[at];
      const size_t nameLength = data[at + 2] | (data[at + 3] << 8U);
      at += 4;
      std::string name;
      uint32_t lastChange = 0;
      if (!padded(nameLength, name) || !card32(lastChange)) {
        return std::nullopt;
      }
      decoded.serials[name] = lastChange;
      if (type == 0) {
        uint32_t value = 0;
        if (!card32(value)) {
          return std::nullopt;
        }
        decoded.settings[name] = static_cast<int32_t>(value);
      } else if (type == 1) {
        uint32_t length = 0;
        std::string value;
        if (!card32(length) || !padded(length, value)) {
          return std::nullopt;
        }
        decoded.settings[name] = value;
      } else {
        return std::nullopt;
      }
    }
    if (at != data.size()) {
      return std::nullopt;
    }
    return decoded;
  }

  int32_t integer(const Decoded& decoded, const std::string& name) {
    const auto found = decoded.settings.find(name);
    return found != decoded.settings.end() && std::holds_alternative<int32_t>(found->second)
        ? std::get<int32_t>(found->second)
        : -1;
  }

} // namespace

// Integer scales reach GTK whole, with fonts at 96 DPI before scaling.
UMBRIEL_TEST(integerScaleIsWholeWindowScale) {
  const auto values = xsettingsFor(3.0, "", 24);
  CHECK_EQ(values.windowScalingFactor, 3);
  CHECK_EQ(values.unscaledDpi, 96 * 1024);
  CHECK_EQ(values.xftDpi, 3 * 96 * 1024);
}

// A fractional scale keeps the integer window scale and carries the rest in the unscaled DPI, so text, like Xft/DPI,
// lands at 2.5x.
UMBRIEL_TEST(fractionalScaleCarriesRemainderInDpi) {
  const auto values = xsettingsFor(2.5, "", 24);
  CHECK_EQ(values.windowScalingFactor, 2);
  CHECK_EQ(values.unscaledDpi, 120 * 1024);
  CHECK_EQ(values.xftDpi, 240 * 1024);
  const auto slight = xsettingsFor(1.25, "", 24);
  CHECK_EQ(slight.windowScalingFactor, 1);
  CHECK_EQ(slight.unscaledDpi, 120 * 1024);
  CHECK_EQ(slight.xftDpi, 120 * 1024);
}

// X clients cannot scale below 1; outputs under scale 1 announce the defaults.
UMBRIEL_TEST(scaleBelowOneAnnouncesDefaults) {
  const auto values = xsettingsFor(0.5, "", 24);
  CHECK_EQ(values.windowScalingFactor, 1);
  CHECK_EQ(values.unscaledDpi, 96 * 1024);
  CHECK_EQ(values.xftDpi, 96 * 1024);
  CHECK_EQ(values.cursorSize, 24);
}

// The cursor size is in X pixels, so it grows with the scale X clients draw at.
UMBRIEL_TEST(cursorSizeFollowsScale) {
  CHECK_EQ(xsettingsFor(2.5, "breeze", 36).cursorSize, 90);
  CHECK_EQ(xsettingsFor(1.0, "breeze", 36).cursorSize, 36);
}

UMBRIEL_TEST(encodedPropertyParses) {
  const auto decoded = decode(encodeXsettings(7, xsettingsFor(2.5, "Adwaita", 24)));
  CHECK(decoded.has_value());
  if (!decoded) {
    return;
  }
  CHECK_EQ(decoded->serial, 7U);
  CHECK_EQ(integer(*decoded, "Gdk/WindowScalingFactor"), 2);
  CHECK_EQ(integer(*decoded, "Gdk/UnscaledDPI"), 120 * 1024);
  CHECK_EQ(integer(*decoded, "Xft/DPI"), 240 * 1024);
  CHECK_EQ(integer(*decoded, "Gtk/CursorThemeSize"), 60);
  CHECK(decoded->settings.at("Gtk/CursorThemeName") == Value(std::string("Adwaita")));
  for (const auto& [name, serial] : decoded->serials) {
    CHECK_EQ(serial, 7U);
  }
}

// Without a configured theme, clients keep their own default rather than an empty name.
UMBRIEL_TEST(unsetCursorThemeIsOmitted) {
  const auto decoded = decode(encodeXsettings(1, xsettingsFor(1.0, "", 24)));
  CHECK(decoded.has_value());
  if (decoded) {
    CHECK(!decoded->settings.contains("Gtk/CursorThemeName"));
    CHECK_EQ(integer(*decoded, "Gtk/CursorThemeSize"), 24);
  }
}

int main() { return RUN_TESTS(); }
