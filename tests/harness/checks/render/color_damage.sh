#!/usr/bin/env bash
# Unchanged color and opacity must deliver requested callbacks without pixel damage.
# Real transitions must repaint, including cached and independent child commits.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"

cat >> "$UMBRIEL_CONFIG" <<'CONFIG'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.blur]
enabled = false
[appearance.shadow]
enabled = false
[output.HEADLESS-1]
hdr = "off"
[[window_rule]]
match.app_id = "^color-feedback-client$"
match.title = "^color-feedback-opaque$"
opacity = 1.0
[[window_rule]]
match.app_id = "^color-feedback-client$"
match.title = "^color-feedback-translucent$"
opacity = 0.95
CONFIG
"$UMBRIEL" msg config-reload > /dev/null
# The inotify reload must finish before capture measurements can observe title-only transitions.
sleep 0.3 # real time: inotify reload timer

readonly CAPTURE_LOG="$UMBRIEL_RUNTIME_DIR/color-damage-capture.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo"
CAPTURE_CONTROL=1 "$UMBRIEL_CAPTURE_CLIENT" < "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo" > "$CAPTURE_LOG" 2>&1 &
exec {CAPTURE_FD}> "$UMBRIEL_RUNTIME_DIR/color-damage-capture.fifo"
await_lines "$CAPTURE_LOG" 'session-ready HEADLESS-1' 1
await_lines "$CAPTURE_LOG" 'frame HEADLESS-1' 1

barrier=0
capture_count() {
  barrier=$((barrier + 1))
  printf 'sync color-%s\n' "$barrier" >&"$CAPTURE_FD"
  await_lines "$CAPTURE_LOG" "synced color-$barrier HEADLESS-1 " 1
  frame_count=$(awk -v label="color-$barrier" '$1 == "synced" && $2 == label && $3 == "HEADLESS-1" { print $4 }' "$CAPTURE_LOG")
  [[ $frame_count =~ ^[0-9]+$ ]]
}

request() {
  local command=$1 previous
  previous=$(events "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"")
  printf '%s\n' "$command" >&"$CLIENT_FD"
  await_lines "$CLIENT_LOG" "{\"event\":\"snapshot\",\"label\":\"$command\"" "$((previous + 1))"
}

expect_frames() {
  local command=$1 expected=$2 description=$3 before after callbacks
  capture_count
  before=$frame_count
  callbacks=$(events "$CLIENT_LOG" '{"event":"frame",')
  request "$command"
  if [[ $command == frame ]]; then
    await_lines "$CLIENT_LOG" '{"event":"frame",' "$((callbacks + 1))"
  fi
  "$UMBRIEL" settle > /dev/null
  capture_count
  after=$frame_count
  if ((after - before != expected)); then
    echo "$mode opacity=$opacity: $description changed capture count $before to $after, expected $expected drawn frame(s)"
    exit 1
  fi
}

for mode in toplevel subsurface subsurface-desync; do
  CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/color-damage-$mode.log"
  fifo="$UMBRIEL_RUNTIME_DIR/color-damage-$mode.fifo"
  mkfifo "$fifo"
  args=()
  [[ $mode == toplevel ]] || args+=("$mode")
  "$UMBRIEL_COLOR_FEEDBACK_CLIENT" "${args[@]}" < "$fifo" > "$CLIENT_LOG" 2>&1 &
  client_pid=$!
  exec {CLIENT_FD}> "$fifo"
  await_lines "$CLIENT_LOG" '{"event":"mapped"}' 1
  "$UMBRIEL" settle > /dev/null
  # The helper initially requests fullscreen, where opaque_fullscreen bypasses rule opacity.
  "$UMBRIEL" msg window-toggle-fullscreen > /dev/null
  "$UMBRIEL" settle > /dev/null
  if ! "$UMBRIEL" tearing --json | jq -e '
    [.surfaces[] | select(.app_id == "color-feedback-client")] as $clients
    | ($clients | length) == 1 and $clients[0].fullscreen == false
  ' > /dev/null; then
    echo "$mode: color client did not leave fullscreen before opacity checks"
    exit 1
  fi
  opacity=1.0
  expect_frames 'title color-feedback-opaque' 0 'a title change retaining opaque content'

  for opacity in 1.0 0.95; do
    if [[ $opacity == 0.95 ]]; then
      expect_frames 'title color-feedback-translucent' 1 'the compositor opacity transition to 0.95'
    fi
    expect_frames frame 0 'a PQ callback-only commit'
    expect_frames commit 0 'an unchanged PQ surface commit'
    expect_frames tag 0 'the same PQ description'
    expect_frames unset 1 'the PQ to sRGB transition'
    expect_frames frame 0 'an sRGB callback-only commit'
    expect_frames commit 0 'an unchanged sRGB surface commit'
    expect_frames unset 0 'the same unset description'
    expect_frames tag 1 'the sRGB to PQ transition'
    expect_frames frame 0 'a callback after restoring PQ'

    if [[ $mode == subsurface ]]; then
      expect_frames cache 0 'a synchronized cached color change'
      expect_frames parent 1 'the cached sRGB state applied by its parent'
      expect_frames apply 1 'the newer pending PQ state applied by its child'
      expect_frames frame 0 'a callback after synchronized state application'
    fi

    expect_frames destroy-only 1 'color-object destruction without a surface commit'
    expect_frames frame 0 'a callback after color-object destruction'
    expect_frames recreate 1 'recreating the color object with PQ'
    expect_frames frame 0 'a callback after color-object recreation'
    echo "$mode opacity=$opacity: unchanged colors stayed quiet, callbacks completed, actual color changes repainted"
  done
  expect_frames 'title color-feedback-opaque' 1 'the compositor opacity transition back to 1.0'

  kill -TERM "$client_pid"
  wait "$client_pid" || true
  exec {CLIENT_FD}>&-
  "$UMBRIEL" settle > /dev/null
done
