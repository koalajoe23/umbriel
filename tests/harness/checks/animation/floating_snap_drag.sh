#!/usr/bin/env bash
# A floating-layout snap animates from the dropped frame while unsnap retains its pre-drag normal origin.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[input]
window_snapping = true
[animation]
duration_ms = 2000
curve = "linear"
[animation.windows_in]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.title_bar]
mode = "forced"
[appearance.shadow]
enabled = false
[colors.title_bar]
background = "#FF0000FF"
background_unfocused = "#FF0000FF"
text = "#FF0000FF"
text_unfocused = "#FF0000FF"
close = "#FF0000FF"
maximize = "#FF0000FF"
[[window_rule]]
match.title = "^floating-snap-motion$"
default_position = { x = 173, y = 109, anchor = "top_left" }
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" msg workspace-set-layout:floating > /dev/null
"$UMBRIEL_FRACTIONAL_CLIENT" floating-snap-motion 480 300 > "$UMBRIEL_RUNTIME_DIR/client.log" 2>&1 &
title_bar_wait_box floating-snap-motion 480x300+173+109
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" clock-freeze > /dev/null
pointer_hold 1280 720 move 300 95 press 272 move 0 360 -- release 272
pointer_release
title_bar_wait_field floating-snap-motion snap left
# The pointer held the title bar 127 px from the left edge and 14 px above content. On release the frame is still
# at (-127, 346), with its content at y=374. Rewinding to the original (+173,+109) would jump before the first tick.
grim "$UMBRIEL_RUNTIME_DIR/released.png"
bounds=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/released.png" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
[[ $bounds == '0 346 353 28' ]] || { echo "snap rewound instead of starting at drop: $bounds"; exit 1; }
"$UMBRIEL" clock-advance 1000 > /dev/null
grim "$UMBRIEL_RUNTIME_DIR/middle.png"
bounds=$("$UMBRIEL_PIXEL_PROBE" "$UMBRIEL_RUNTIME_DIR/middle.png" bbox 'r > 0.9 && g < 0.1 && b < 0.1')
read -r x y width height <<< "$bounds"
# Halfway from (-127,346,480) to (8,8,628), clipped at the output's left edge.
((x == 0 && y >= 176 && y <= 178 && width >= 493 && width <= 495 && height == 28)) || {
  echo "snap did not interpolate from the dropped frame: $bounds"; exit 1;
}
"$UMBRIEL" clock-advance 2000 > /dev/null
"$UMBRIEL" settle > /dev/null
title_bar_wait_box floating-snap-motion 628x676+8+36
"$UMBRIEL" msg window-unsnap > /dev/null
"$UMBRIEL" clock-advance 3000 > /dev/null
"$UMBRIEL" settle > /dev/null
title_bar_wait_box floating-snap-motion 480x300+173+109
"$UMBRIEL" clock-resume > /dev/null
echo "floating snap animates from the dropped window and restores its original normal box"
