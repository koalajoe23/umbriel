#!/usr/bin/env bash
# Destroying an output rehomes the windows of its hidden workspaces while wlroots is still tearing the output down. A
# window that was never shown on that output must not be entered onto it then: the enter registers output listeners
# that the teardown never reaches, and wlroots aborts on them (a VT switch takes every monitor this way).
set -euo pipefail

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[output.HEADLESS-1]
workspaces = 2

[[window_rule]]
match.app_id = "^destroy-hidden$"
default_workspace = 2
default_focused = false
EOF
"$UMBRIEL" msg config-reload > /dev/null

workspace_of() {
  "$UMBRIEL" windows --json | jq -r '.[] | select(.app_id == "destroy-hidden") | .workspace'
}

wait_for_workspace() {
  local expected=$1 workspace=
  for _ in $(seq 40); do
    workspace=$(workspace_of)
    [[ $workspace == "$expected" ]] && return 0
    sleep 0.1
  done
  echo "expected the window on workspace '$expected', got '$workspace'"
  return 1
}

foot --app-id=destroy-hidden sh -c 'sleep 120' > /dev/null 2>&1 &
wait_for_workspace HEADLESS-1:2
"$UMBRIEL" settle > /dev/null

"$UMBRIEL" output-destroy HEADLESS-1 > /dev/null
wait_for_workspace ''

echo "destroying an output kept a never-shown hidden window and the compositor alive"
