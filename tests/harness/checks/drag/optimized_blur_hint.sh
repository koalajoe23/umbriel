#!/usr/bin/env bash
# A dragged tile with optimized blur still shows the insert hint drawn below it: the drag samples the live scene
# instead of the cached background blur, which never contains the hint.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BTN_LEFT=272
readonly OUTPUT_W=1280
readonly OUTPUT_H=720
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/optimized-blur-hint.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout.scrolling]
default_extent_fraction = 0.5
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
drag_opacity = 1.0
[appearance.shadow]
enabled = false
[appearance.blur]
enabled = true
optimized = true
passes = 2
radius = 4
noise = 0.0
brightness = 1.0
contrast = 1.0
saturation = 1.0
[colors]
backdrop = "#00FF00FF"
insert_hint = "#FF0000FF"
[[window_rule]]
match.title = "^blur-hint-"
blur = true
blur_optimized = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

for name in a b; do
  FILL_COLOR=0x00000000 "$UMBRIEL_UNMAP_CLIENT" "blur-hint-$name" 600 600 \
    > "$UMBRIEL_RUNTIME_DIR/blur-hint-$name.log" 2>&1 &
  await_lines "$UMBRIEL_RUNTIME_DIR/blur-hint-$name.log" mapped 1
done
"$UMBRIEL" settle

read -r ax ay aw ah < <("$UMBRIEL" windows --json \
  | jq -r '.[] | select(.title == "blur-hint-a") | "\(.x) \(.y) \(.w) \(.h)"')
read -r bx by bw bh < <("$UMBRIEL" windows --json \
  | jq -r '.[] | select(.title == "blur-hint-b") | "\(.x) \(.y) \(.w) \(.h)"')
start_x=$((ax + aw / 2))
start_y=$((ay + ah / 2))
drop_x=$((bx + bw / 2))
drop_y=$((by + bh / 2))

pointer_hold "$OUTPUT_W" "$OUTPUT_H" move "$start_x" "$start_y" mod super press "$BTN_LEFT" \
  move $(((start_x + drop_x) / 2)) "$drop_y" move "$drop_x" "$drop_y" -- release "$BTN_LEFT" mod none
"$UMBRIEL" settle
grim "$IMAGE"
# The grab keeps the pointer at the window's center, so the dragged window covers 300 px around the drop point, and
# the hint fills b's column from its center. Their overlap, kept clear of the blur's edge falloff:
region="200x400+$((drop_x + 40))+$((drop_y - 200))"
red=$("$UMBRIEL_PIXEL_PROBE" "$IMAGE" count 'r > 0.9 && g < 0.1' "$region")
if ((red < 200 * 400 * 9 / 10)); then
  echo "the insert hint was not visible through the dragged window's optimized blur: $red red pixels in $region"
  exit 1
fi
pointer_release

echo "the dragged window's optimized blur showed the insert hint below it"
