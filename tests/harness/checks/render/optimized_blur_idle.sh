#!/usr/bin/env bash
# Background frame callbacks leave optimized blur idle, while parent pixels, desynchronized child pixels, child
# placement, and layer geometry still update the cached backdrop seen through a translucent window.
set -euo pipefail

source "$UMBRIEL_HARNESS_LIB"

readonly LAYER_LOG="$UMBRIEL_RUNTIME_DIR/blur-idle-layer.log"
readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/blur-idle-capture.log"
readonly IMAGE="$UMBRIEL_RUNTIME_DIR/blur-idle.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
enabled = false
[appearance]
border_width = 0
corner_radius = 0
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
[[window_rule]]
match.title = "^optimized-blur-idle$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
blur = true
blur_optimized = true
EOF
"$UMBRIEL" msg config-reload > /dev/null

mkfifo "$UMBRIEL_RUNTIME_DIR/layer.fifo" "$UMBRIEL_RUNTIME_DIR/capture.fifo"
env LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 \
  < "$UMBRIEL_RUNTIME_DIR/layer.fifo" > "$LAYER_LOG" 2>&1 &
exec {layer_fd}> "$UMBRIEL_RUNTIME_DIR/layer.fifo"
await_lines "$LAYER_LOG" ready 1

env TRANSLUCENT_CONTENT=1 "$UMBRIEL_SUBSURFACE_CLIENT" optimized-blur-idle 900 600 \
  > "$UMBRIEL_RUNTIME_DIR/blur-idle-window.log" 2>&1 &
await_lines "$UMBRIEL_RUNTIME_DIR/blur-idle-window.log" mapped 1
"$UMBRIEL" settle > /dev/null

env CAPTURE_CONTROL=1 "$UMBRIEL_CAPTURE_CLIENT" --output HEADLESS-1 \
  < "$UMBRIEL_RUNTIME_DIR/capture.fifo" > "$CAPTURE_LOG" 2>&1 &
exec {capture_fd}> "$UMBRIEL_RUNTIME_DIR/capture.fifo"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-1' 1
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1

layer_command() {
  local action=$1 label=$2 count=1
  shift 2
  [[ $action == frames ]] && count=$1
  printf '%s %s %s\n' "$action" "$label" "$*" >&"$layer_fd"
  await_lines "$LAYER_LOG" "done $label callbacks $count" 1
  "$UMBRIEL" settle > /dev/null
}

capture_count() {
  local label=$1
  printf 'sync %s\n' "$label" >&"$capture_fd"
  await_lines "$CAPTURE_LOG" "synced $label HEADLESS-1" 1
  awk -v label="$label" '$1 == "synced" && $2 == label && $3 == "HEADLESS-1" { print $4 }' "$CAPTURE_LOG"
}

sample() {
  grim "$IMAGE"
  "$UMBRIEL_PIXEL_PROBE" "$IMAGE" pixel 150 150
}

require_change() {
  local before=$1 label=$2
  local after
  after=$(capture_count "$label")
  ((after > before)) || { echo "$label did not deliver changed backdrop pixels"; exit 1; }
}

layer_command color red FFFF0000
read -r red_r red_g red_b < <(sample)
before=$(capture_count before-callbacks)
layer_command frames callbacks 32
after=$(capture_count after-callbacks)
((after == before)) || { echo "callback-only background commits repainted optimized blur: $before to $after"; exit 1; }

layer_command color green FF00FF00
require_change "$after" parent-damage
read -r green_r green_g green_b < <(sample)
((red_r > green_r + 40 && green_g > red_g + 80)) || {
  echo "parent damage left stale blur: red=$red_r,$red_g,$red_b green=$green_r,$green_g,$green_b"
  exit 1
}

before=$(capture_count before-child-map)
layer_command child-color child-blue FF0000FF
require_change "$before" child-map
read -r blue_r blue_g blue_b < <(sample)
((green_g > blue_g + 80 && blue_b > green_b + 40)) || {
  echo "desynchronized child map left stale blur: green=$green_r,$green_g,$green_b blue=$blue_r,$blue_g,$blue_b"
  exit 1
}

before=$(capture_count before-child-damage)
layer_command child-color child-red FFFF0000
require_change "$before" child-damage
read -r child_r child_g child_b < <(sample)
((child_r > blue_r + 40 && blue_b > child_b + 40)) || {
  echo "desynchronized child damage left stale blur: blue=$blue_r,$blue_g,$blue_b red=$child_r,$child_g,$child_b"
  exit 1
}

before=$(capture_count before-child-move)
layer_command child-move child-moved 500 0
require_change "$before" child-move
read -r moved_r moved_g moved_b < <(sample)
((moved_g > child_g + 80)) || { echo "moving the child left stale blur at its old position"; exit 1; }

layer_command color geometry-red FFFF0000
read -r full_r full_g full_b < <(sample)
before=$(capture_count before-geometry)
layer_command margins geometry 250 0 0 0
require_change "$before" layer-geometry
read -r margin_r margin_g margin_b < <(sample)
((full_r > margin_r + 40 && margin_g > full_g + 80)) || {
  echo "layer geometry left stale blur: full=$full_r,$full_g,$full_b margin=$margin_r,$margin_g,$margin_b"
  exit 1
}

echo 'optimized blur ignores callback-only commits and follows parent, child, placement, and layer geometry changes'
