#!/usr/bin/env bash
# Protocol errors disconnect only the offender. Source destruction, role
# destruction, and client disconnect must also release the carried window.
set -euo pipefail
source "$UMBRIEL_HARNESS_LIB"
cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[[window_rule]]
match.title = "^toplevel-drag-source$"
default_floating = true
default_position = { x = 100, y = 100, anchor = "top_left" }
EOF
"$UMBRIEL" msg config-reload > /dev/null

for operation in a d b c s t disconnect; do
  log="$UMBRIEL_RUNTIME_DIR/toplevel-lifecycle-$operation.log"
  fifo="$UMBRIEL_RUNTIME_DIR/toplevel-lifecycle-$operation.fifo"
  mkfifo "$fifo"
  "$UMBRIEL_DRAG_CLIENT" toplevel < "$fifo" > "$log" 2>&1 &
  client_pid=$!
  exec {control_fd}> "$fifo"
  await_lines "$log" ready 1
  "$UMBRIEL" settle > /dev/null
  pointer_hold 1280 720 move 200 200 press 272 -- release 272
  await_lines "$log" tear-off-mapped 1
  case $operation in
    a|d|b|c)
      printf '%s\n' "$operation" >&"$control_fd"
      interface=xdg_toplevel_drag_v1
      code=0
      if [[ $operation == d ]]; then
        interface=none
        code=1
      fi
      [[ $operation == b ]] && interface=xdg_toplevel_drag_manager_v1
      await_lines "$log" "protocol-error $interface $code" 1
      if [[ $operation == d ]]; then
        grep -q 'drag has not ended' "$log"
      elif [[ $operation == c ]]; then
        grep -q 'toplevel drag source used as selection' "$log"
      fi
      if wait "$client_pid"; then
        echo "$operation: invalid protocol request succeeded"; exit 1
      fi
      ;;
    s)
      printf 's\n' >&"$control_fd"
      await_lines "$log" 'command s' 1
      "$UMBRIEL" settle > /dev/null
      "$UMBRIEL" windows --json | jq -e 'any(.[]; .title == "toplevel-drag-window" and .column >= 0)' > /dev/null
      printf 'dq\n' >&"$control_fd"
      wait "$client_pid"
      ;;
    t)
      printf 't\n' >&"$control_fd"
      await_lines "$log" 'command t' 1
      "$UMBRIEL" settle > /dev/null
      "$UMBRIEL" windows --json | jq -e 'all(.[]; .title != "toplevel-drag-window")' > /dev/null
      ;;
    disconnect)
      kill "$client_pid"
      wait "$client_pid" || true
      ;;
  esac
  pointer_release
  if [[ $operation == t ]]; then
    await_lines "$log" drag-cancelled 1
    printf 'dq\n' >&"$control_fd"
    wait "$client_pid"
  fi
  exec {control_fd}>&-
  "$UMBRIEL" settle > /dev/null
  [[ $("$UMBRIEL" windows --json | jq length) == 0 ]] || { echo "$operation: client cleanup left windows"; exit 1; }
  "$UMBRIEL_GLOBAL_CLIENT" xdg_toplevel_drag_manager_v1 present 1
  echo "$operation: lifecycle and compositor survival verified"
done
