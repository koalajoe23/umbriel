#!/usr/bin/env bash
# harness: outputs=2
# Per-output SDR transfer choices survive reload independently. Headless outputs
# cannot enter PQ, so both choices must leave their SDR fallback pixels intact.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

readonly BASELINE=$(< "$UMBRIEL_CONFIG")
readonly LEFT_LOG="$UMBRIEL_RUNTIME_DIR/sdr-transfer-left.log"
readonly RIGHT_LOG="$UMBRIEL_RUNTIME_DIR/sdr-transfer-right.log"
readonly LEFT_BASE="$UMBRIEL_RUNTIME_DIR/sdr-transfer-left-base.png"
readonly RIGHT_BASE="$UMBRIEL_RUNTIME_DIR/sdr-transfer-right-base.png"
readonly LEFT_IMAGE="$UMBRIEL_RUNTIME_DIR/sdr-transfer-left.png"
readonly RIGHT_IMAGE="$UMBRIEL_RUNTIME_DIR/sdr-transfer-right.png"

write_config() {
  local left=$1 right=$2
  {
    printf '%s\n' "$BASELINE"
    cat <<'CONFIG'

[animation]
enabled = false
[appearance.blur]
enabled = false
[output.HEADLESS-1]
hdr = "on"
CONFIG
    [[ -z $left ]] || printf 'sdr_transfer = "%s"\n' "$left"
    printf '\n[output.HEADLESS-2]\nhdr = "on"\n'
    [[ -z $right ]] || printf 'sdr_transfer = "%s"\n' "$right"
  } > "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  "$UMBRIEL" settle > /dev/null
}

expect_choices() {
  local left=$1 right=$2 color
  color=$("$UMBRIEL" color --json)
  if ! jq -e --arg left "$left" --arg right "$right" '
    .outputs | length == 2
      and all(.[]; .hdr_mode == "on" and .hdr_requested and (.hdr_active | not)
        and .transfer_function == "none" and .primaries == "none"
        and (.fallback_reason | contains("does not advertise PQ")))
      and any(.[]; .name == "HEADLESS-1" and .sdr_transfer == $left)
      and any(.[]; .name == "HEADLESS-2" and .sdr_transfer == $right)
  ' <<< "$color" > /dev/null; then
    echo "SDR transfer reload did not report $left/$right with HDR fallback: $color"
    exit 1
  fi
}

layer_command() {
  local descriptor=$1 log=$2 label=$3 color=$4
  printf 'color %s %s\n' "$label" "$color" >&"$descriptor"
  await_lines "$log" "done $label callbacks 1" 1
  "$UMBRIEL" settle > /dev/null
}

expect_dark_pixel() {
  local image=$1 level=$2 label=$3 red green blue
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel 640 360)
  if ((red != level || green != level || blue != level)); then
    echo "$label: SDR fallback rendered $red $green $blue, expected $level per channel"
    exit 1
  fi
}

expect_same_image() {
  local image=$1 baseline=$2 label=$3 maximum changed
  read -r maximum changed < <("$UMBRIEL_PIXEL_PROBE" "$image" diff "$baseline" 400x300+400+250)
  if ((maximum != 0 || changed != 0)); then
    echo "$label: the SDR transfer choice changed fallback pixels, maximum=$maximum changed=$changed"
    exit 1
  fi
}

expect_fallback_pixels() {
  local label=$1
  grim -o HEADLESS-1 "$LEFT_IMAGE"
  grim -o HEADLESS-2 "$RIGHT_IMAGE"
  expect_dark_pixel "$LEFT_IMAGE" 16 "$label left output"
  expect_dark_pixel "$RIGHT_IMAGE" 24 "$label right output"
  expect_same_image "$LEFT_IMAGE" "$LEFT_BASE" "$label left output"
  expect_same_image "$RIGHT_IMAGE" "$RIGHT_BASE" "$label right output"
}

write_config '' ''
expect_choices gamma22 gamma22

mkfifo "$UMBRIEL_RUNTIME_DIR/sdr-transfer-left.fifo" "$UMBRIEL_RUNTIME_DIR/sdr-transfer-right.fifo"
LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-1 0 \
  < "$UMBRIEL_RUNTIME_DIR/sdr-transfer-left.fifo" > "$LEFT_LOG" 2>&1 &
exec {left_fd}> "$UMBRIEL_RUNTIME_DIR/sdr-transfer-left.fifo"
LAYER_CONTROL=1 "$UMBRIEL_LAYER_CLIENT" HEADLESS-2 0 \
  < "$UMBRIEL_RUNTIME_DIR/sdr-transfer-right.fifo" > "$RIGHT_LOG" 2>&1 &
exec {right_fd}> "$UMBRIEL_RUNTIME_DIR/sdr-transfer-right.fifo"
await_lines "$LEFT_LOG" ready 1
await_lines "$RIGHT_LOG" ready 1
layer_command "$left_fd" "$LEFT_LOG" left-dark FF101010
layer_command "$right_fd" "$RIGHT_LOG" right-dark FF181818
grim -o HEADLESS-1 "$LEFT_BASE"
grim -o HEADLESS-2 "$RIGHT_BASE"
expect_dark_pixel "$LEFT_BASE" 16 'default left output'
expect_dark_pixel "$RIGHT_BASE" 24 'default right output'

# Each reload changes only one output while retaining the other output's choice.
write_config srgb ''
expect_choices srgb gamma22
expect_fallback_pixels left-srgb
write_config srgb srgb
expect_choices srgb srgb
expect_fallback_pixels both-srgb
write_config gamma22 srgb
expect_choices gamma22 srgb
expect_fallback_pixels left-gamma22
write_config '' ''
expect_choices gamma22 gamma22
expect_fallback_pixels defaults-restored

# A real client update must change its pixels, proving the unchanged captures
# above came from a live scene rather than an old capture buffer.
layer_command "$left_fd" "$LEFT_LOG" left-updated FF303030
grim -o HEADLESS-1 "$LEFT_IMAGE"
grim -o HEADLESS-2 "$RIGHT_IMAGE"
expect_dark_pixel "$LEFT_IMAGE" 48 'left client update'
expect_same_image "$RIGHT_IMAGE" "$RIGHT_BASE" 'right output after left client update'

echo "SDR transfer choices reloaded independently, reset to gamma22, and preserved live HDR-fallback pixels"
