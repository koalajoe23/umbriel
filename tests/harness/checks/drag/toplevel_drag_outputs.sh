#!/usr/bin/env bash
# harness: outputs=2
# The attached window spans outputs during the drag, uses logical offsets on
# a scaled output, and joins the destination workspace only after the drop.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'EOF'
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
enabled = false
[output.HEADLESS-1]
position = [0, 0]
scale = 1
[output.HEADLESS-2]
position = [1280, 0]
scale = 2
[[window_rule]]
match.title = "^toplevel-drag-source$"
default_output = "HEADLESS-1"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
[[window_rule]]
match.title = "^toplevel-drag-target$"
default_output = "HEADLESS-2"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null
target_log="$UMBRIEL_RUNTIME_DIR/toplevel-output-target.log"
source_log="$UMBRIEL_RUNTIME_DIR/toplevel-output-source.log"
mkfifo "$UMBRIEL_RUNTIME_DIR/toplevel-output.fifo"
"$UMBRIEL_DRAG_CLIENT" target toplevel-drag-target > "$target_log" 2>&1 &
target_pid=$!
await_lines "$target_log" ready 1
"$UMBRIEL_DRAG_CLIENT" toplevel < "$UMBRIEL_RUNTIME_DIR/toplevel-output.fifo" > "$source_log" 2>&1 &
source_pid=$!
exec {control_fd}> "$UMBRIEL_RUNTIME_DIR/toplevel-output.fifo"
await_lines "$source_log" ready 1
"$UMBRIEL" settle > /dev/null
pointer_hold 1920 720 move 200 200 press 272 -- move 1260 200 mark spanning hold move 1700 200 mark target hold release 272
await_lines "$source_log" tear-off-mapped 1
pointer_step spanning
"$UMBRIEL" settle > /dev/null
for output in HEADLESS-1 HEADLESS-2; do
  image="$UMBRIEL_RUNTIME_DIR/toplevel-$output.png"
  grim -o "$output" "$image"
  x=1260
  y=250
  if [[ $output == HEADLESS-2 ]]; then
    x=40
    y=500
  fi
  read -r red green blue < <("$UMBRIEL_PIXEL_PROBE" "$image" pixel "$x" "$y")
  ((red > green + 40 && blue > green + 15)) || { echo "carried window clipped off $output: $red,$green,$blue"; exit 1; }
done
pointer_step target
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "toplevel-drag-window" and .x == 1680 and .y == 170 and .column == -1)' > /dev/null
await_lines "$target_log" drag-enter 1
pointer_release
await_lines "$target_log" drop-received 1
await_lines "$source_log" drag-finished 1
wait "$target_pid"
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "toplevel-drag-window" and .column >= 0 and (.workspace | startswith("HEADLESS-2:")))' > /dev/null
printf 'dq\n' >&"$control_fd"
wait "$source_pid"
exec {control_fd}>&-
echo 'toplevel drag spans outputs and drops onto the scaled destination workspace'
