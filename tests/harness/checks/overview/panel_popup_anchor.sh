#!/usr/bin/env bash
# Release-driven panel menus are dispatched at wl_pointer.frame. A final release over a panel in overview must keep
# its pointer position valid until that frame, while releasing an implicit grab over the inert desktop clears it.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly LAYER="${UMBRIEL_LAYER_CLIENT:-./build-debug/tests/layer-client}"
readonly POINTER="${UMBRIEL_POINTER_CLIENT:-./build-debug/tests/pointer-client}"
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly LEFT_BUTTON=272

if [[ ! -x $LAYER || ! -x $POINTER ]]; then
  echo "panel popup helpers are not available"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false

[appearance]
corner_radius = 0

[appearance.shadow]
enabled = false

[overview]
background_blur = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

start_panel() {
  local phase=$1
  shift
  PANEL_LOG="$UMBRIEL_RUNTIME_DIR/$phase.log"
  "$LAYER" HEADLESS-1 40 popup-on-release "$@" > "$PANEL_LOG" 2>&1 &
  PANEL_PID=$!
  await_events "$PANEL_LOG" ready 1 "$phase panel"
}

stop_panel() {
  kill "$PANEL_PID"
  wait "$PANEL_PID" 2>/dev/null || true
  "$UMBRIEL" settle
}

check_menu() {
  local phase=$1 x=$2 y=$3
  "$POINTER" "$OUTPUT_W" "$OUTPUT_H" move "$x" "$y" click "$LEFT_BUTTON" > /dev/null
  await_events "$PANEL_LOG" popup-mapped 1 "$phase popup"

  # This is the transition that broke Noctalia's inline tray: the compositor sent a leave after the release but before
  # its frame, so the client's batched release handler had lost the pointer position and opened its menu at zero.
  if ! grep -qx "pointer-release-frame valid=true x=$x y=$y" "$PANEL_LOG"; then
    echo "$phase lost its pointer anchor before dispatching the release: $(tr '\n' '|' < "$PANEL_LOG")"
    exit 1
  fi
  if ! grep -qx "popup-configure x=$x y=$((y + 1)) width=80 height=80" "$PANEL_LOG"; then
    echo "$phase configured its popup away from the click: $(tr '\n' '|' < "$PANEL_LOG")"
    exit 1
  fi

  "$UMBRIEL" settle
  local image="$UMBRIEL_RUNTIME_DIR/$phase.png" bx by bw bh
  grim "$image"
  read -r bx by bw bh < <("$UMBRIEL_PIXEL_PROBE" "$image" bbox 'r > 0.95 && g > 0.6 && g < 0.75 && b < 0.05')
  if ((bx != x || by != y + 1 || bw != 80 || bh != 80)); then
    echo "$phase rendered its popup at $bx,$by ${bw}x$bh, expected $x,$((y + 1)) 80x80"
    exit 1
  fi
  stop_panel
}

# The desktop control establishes the client's real release/frame/popup path before overview changes input routing.
start_panel desktop
check_menu desktop 1160 20

"$UMBRIEL" msg overview-open > /dev/null
"$UMBRIEL" settle
start_panel overview-top
check_menu overview-top 1160 20

# Overlay surfaces use the same passthrough policy. This fixture is a 200x200 square at the output's top-left.
start_panel overview-overlay overlay-layer
check_menu overview-overlay 160 20

# A press on the panel owns an implicit grab even after moving into the overview. Its final release must reach that
# panel, then clear focus before the batched handler runs because the desktop underneath remains inert.
start_panel overview-outside
"$POINTER" "$OUTPUT_W" "$OUTPUT_H" move 1160 20 press "$LEFT_BUTTON" move 1160 300 release "$LEFT_BUTTON" > /dev/null
await_events "$PANEL_LOG" pointer-release-frame 1 "the release outside the panel"
if ! grep -qx 'pointer-release-frame valid=false x=0 y=0' "$PANEL_LOG"; then
  echo "releasing over the inert desktop retained panel pointer focus: $(tr '\n' '|' < "$PANEL_LOG")"
  exit 1
fi
stop_panel

echo "release-driven top and overlay panel menus keep their pointer anchor in overview, and outside releases clear it"
