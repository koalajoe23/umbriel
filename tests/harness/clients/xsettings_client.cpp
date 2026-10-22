// Prints the XSETTINGS manager's settings on DISPLAY as one line, `serial=N name=value ...` with names sorted, or
// `none` while no client owns _XSETTINGS_S0. Exits 1 when the display or the property cannot be read.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <map>
#include <print>
#include <string>
#include <string_view>
#include <xcb/xcb.h>

namespace {
  xcb_atom_t internAtom(xcb_connection_t* connection, std::string_view name) {
    xcb_intern_atom_reply_t* reply = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, 0, static_cast<uint16_t>(name.size()), name.data()), nullptr
    );
    const xcb_atom_t atom = reply != nullptr ? reply->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
    std::free(reply);
    return atom;
  }

  class Reader {
  public:
    Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

    bool card32(uint32_t& out) {
      if (m_at + 4 > m_size) {
        return false;
      }
      out = m_data[m_at]
          | (m_data[m_at + 1] << 8U)
          | (m_data[m_at + 2] << 16U)
          | (static_cast<uint32_t>(m_data[m_at + 3]) << 24U);
      m_at += 4;
      return true;
    }

    bool padded(size_t length, std::string& out) {
      const size_t span = length + ((4 - (length % 4)) % 4);
      if (m_at + span > m_size) {
        return false;
      }
      out.assign(reinterpret_cast<const char*>(m_data + m_at), length);
      m_at += span;
      return true;
    }

    bool header(uint8_t& type, std::string& name) {
      if (m_at + 4 > m_size) {
        return false;
      }
      type = m_data[m_at];
      const size_t length = m_data[m_at + 2] | (m_data[m_at + 3] << 8U);
      m_at += 4;
      uint32_t lastChange = 0;
      return padded(length, name) && card32(lastChange);
    }

    void skip(size_t bytes) { m_at += bytes; }

  private:
    const uint8_t* m_data;
    size_t m_size;
    size_t m_at = 0;
  };
} // namespace

int main() {
  xcb_connection_t* connection = xcb_connect(nullptr, nullptr);
  if (xcb_connection_has_error(connection) != 0) {
    std::println(stderr, "cannot connect to DISPLAY");
    return 1;
  }
  const xcb_atom_t selection = internAtom(connection, "_XSETTINGS_S0");
  const xcb_atom_t property = internAtom(connection, "_XSETTINGS_SETTINGS");
  xcb_get_selection_owner_reply_t* owner =
      xcb_get_selection_owner_reply(connection, xcb_get_selection_owner(connection, selection), nullptr);
  const xcb_window_t window = owner != nullptr ? owner->owner : static_cast<xcb_window_t>(XCB_WINDOW_NONE);
  std::free(owner);
  if (window == XCB_WINDOW_NONE) {
    std::println("none");
    xcb_disconnect(connection);
    return 0;
  }
  xcb_get_property_reply_t* reply = xcb_get_property_reply(
      connection, xcb_get_property(connection, 0, window, property, property, 0, 65536), nullptr
  );
  if (reply == nullptr || reply->format != 8) {
    std::println(stderr, "the settings property is missing");
    std::free(reply);
    xcb_disconnect(connection);
    return 1;
  }
  const auto* data = static_cast<const uint8_t*>(xcb_get_property_value(reply));
  const auto size = static_cast<size_t>(xcb_get_property_value_length(reply));
  Reader reader(data, size);
  uint32_t serial = 0;
  uint32_t count = 0;
  reader.skip(4);
  bool valid = size >= 12 && data[0] == 0 && reader.card32(serial) && reader.card32(count);
  std::map<std::string, std::string> settings;
  for (uint32_t i = 0; valid && i < count; ++i) {
    uint8_t type = 0;
    std::string name;
    uint32_t value = 0;
    std::string text;
    valid = reader.header(type, name);
    if (valid && type == 0) {
      valid = reader.card32(value);
      settings[name] = std::to_string(static_cast<int32_t>(value));
    } else if (valid && type == 1) {
      valid = reader.card32(value) && reader.padded(value, text);
      settings[name] = text;
    } else {
      valid = false;
    }
  }
  std::free(reply);
  xcb_disconnect(connection);
  if (!valid) {
    std::println(stderr, "malformed settings property");
    return 1;
  }
  std::string line = std::format("serial={}", serial);
  for (const auto& [name, value] : settings) {
    line += std::format(" {}={}", name, value);
  }
  std::println("{}", line);
  return 0;
}
