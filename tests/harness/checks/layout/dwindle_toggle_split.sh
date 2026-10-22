#!/usr/bin/env bash
# layout-toggle-split flips the focused tile's Dwindle split between side by side and stacked, and the flipped split
# stays put through later arranges even with preserve_split off. It is refused outside the Dwindle layout.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"

spawn_client() {
  local title=$1
  "$CLIENT" "$title" 1200 700 > "$UMBRIEL_RUNTIME_DIR/$title.log" 2>&1 &
}

wait_for_count() {
  local want=$1
  for _ in $(seq 60); do
    [[ $("$UMBRIEL" windows --json | jq 'length') -eq $want ]] && return 0
    sleep 0.1
  done
  echo "expected $want windows, got: $("$UMBRIEL" windows --json)"
  return 1
}

# Polls until the two windows are stacked (same x, different y) or side by side (same y, different x).
wait_for_arrangement() {
  local want=$1 a=$2 b=$3 windows ax ay bx by
  for _ in $(seq 40); do
    windows=$("$UMBRIEL" windows --json)
    ax=$(jq -r --arg t "$a" '.[] | select(.title == $t) | .x' <<< "$windows")
    ay=$(jq -r --arg t "$a" '.[] | select(.title == $t) | .y' <<< "$windows")
    bx=$(jq -r --arg t "$b" '.[] | select(.title == $t) | .x' <<< "$windows")
    by=$(jq -r --arg t "$b" '.[] | select(.title == $t) | .y' <<< "$windows")
    if [[ $want == stacked && $ax == "$bx" && $ay != "$by" ]]; then
      return 0
    fi
    if [[ $want == side-by-side && $ay == "$by" && $ax != "$bx" ]]; then
      return 0
    fi
    sleep 0.1
  done
  echo "expected $a and $b $want: $windows"
  return 1
}

# Outside Dwindle the action is refused.
spawn_client toggle-split-a
wait_for_count 1
if "$UMBRIEL" msg layout-toggle-split > /dev/null 2>&1; then
  echo "layout-toggle-split was accepted outside the Dwindle layout"
  exit 1
fi
echo "layout-toggle-split is refused outside Dwindle"

cat >> "$UMBRIEL_CONFIG" <<'CFG'

[layout]
mode = "dwindle"

[layout.dwindle]
preserve_split = false

[animation]
duration_ms = 1
curve = "linear"
CFG
"$UMBRIEL" msg config-reload > /dev/null

spawn_client toggle-split-b
wait_for_count 2
wait_for_arrangement side-by-side toggle-split-a toggle-split-b

"$UMBRIEL" msg layout-toggle-split > /dev/null
wait_for_arrangement stacked toggle-split-a toggle-split-b
echo "layout-toggle-split stacks a side by side pair"

# A third window and a config reload each arrange again; the toggled split must not flip back.
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" settle
wait_for_arrangement stacked toggle-split-a toggle-split-b
spawn_client toggle-split-c
wait_for_count 3
"$UMBRIEL" settle
wait_for_arrangement stacked toggle-split-a toggle-split-b
echo "A toggled split survives arrange with preserve_split off"
