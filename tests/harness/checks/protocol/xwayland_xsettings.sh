#!/usr/bin/env bash
# harness: xwayland=native
# harness: xcursor-theme=true
# X11 clients read the interface scale and cursor theme from Umbriel's XSETTINGS manager. At native resolution the
# scale is the output's, split into an integer window scale and the fractional rest in the unscaled DPI, and both an
# output scale change and a cursor change republish the settings under a new serial.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly SETTINGS_CLIENT="${UMBRIEL_XSETTINGS_CLIENT:-./build-debug/tests/xsettings-client}"
readonly X_CLIENT="${UMBRIEL_XWAYLAND_FOCUS_CLIENT:-./build-debug/tests/xwayland-focus-client}"
readonly X_CONTROL="$UMBRIEL_RUNTIME_DIR/xsettings-control"

if [[ ! -x $SETTINGS_CLIENT || ! -x $X_CLIENT || -z ${DISPLAY:-} ]]; then
  echo "the XSETTINGS helpers or the private DISPLAY are not available"
  exit 1
fi

# A mapped X11 window keeps the lazy Xwayland, and with it the serial, alive between reads.
mkfifo "$X_CONTROL"
exec {x_control_fd}<>"$X_CONTROL"
"$X_CLIENT" xsettings-holder <&"$x_control_fd" > "$UMBRIEL_RUNTIME_DIR/xsettings-holder.log" 2>&1 &

serial=0
last=
# Waits until the settings equal $1, then requires a serial newer than the previous one.
await_settings() {
  local expected=$1
  for _ in $(seq 100); do
    last=$("$SETTINGS_CLIENT" 2>&1) || true
    if [[ ${last#* } == "$expected" && $last == serial=* ]]; then
      local current=${last%% *}
      current=${current#serial=}
      if ((current <= serial)); then
        echo "settings '$expected' arrived without a newer serial than $serial: $last"
        return 1
      fi
      serial=$current
      return 0
    fi
    sleep 0.02
  done
  echo "XSETTINGS never became '$expected', last read: '$last'"
  return 1
}

await_settings "Gdk/UnscaledDPI=98304 Gdk/WindowScalingFactor=1 Gtk/CursorThemeName=umbriel-harness Gtk/CursorThemeSize=24 Xft/DPI=98304"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output."HEADLESS-1"]
scale = 2.5
EOF
"$UMBRIEL" msg config-reload > /dev/null
await_settings "Gdk/UnscaledDPI=122880 Gdk/WindowScalingFactor=2 Gtk/CursorThemeName=umbriel-harness Gtk/CursorThemeSize=60 Xft/DPI=245760"

sed -i 's/^size = 24$/size = 48/' "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_settings "Gdk/UnscaledDPI=122880 Gdk/WindowScalingFactor=2 Gtk/CursorThemeName=umbriel-harness Gtk/CursorThemeSize=120 Xft/DPI=245760"

echo "XSETTINGS announces the native output scale and cursor theme and follows scale and cursor reloads"
