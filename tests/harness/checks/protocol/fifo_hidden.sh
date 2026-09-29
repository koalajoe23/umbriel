#!/usr/bin/env bash
# fifo-v1 on a hidden surface: with the window's workspace switched away there are no frame callbacks to clear the
# barrier, so the hidden-surface tick must keep a barrier burst draining. Each commit a later one supersedes resolves
# as discarded while nothing shows it.
set -euo pipefail

readonly CLIENT="$UMBRIEL_PACING_CLIENT"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/fifo-hidden.log"
readonly CLIENT_INPUT="$UMBRIEL_RUNTIME_DIR/fifo-hidden.in"

if [[ ! -x $CLIENT ]]; then
  echo "pacing-client is not built"
  exit 1
fi

wait_log() {
  local pattern=$1 expected=${2:-1} count
  for _ in $(seq 400); do
    count=$(grep -c -- "$pattern" "$CLIENT_LOG" || true)
    ((count >= expected)) && return 0
    sleep 0.025
  done
  echo "timed out waiting for $expected '$pattern' lines: $(tr '\n' '|' < "$CLIENT_LOG")"
  return 1
}

mkfifo "$CLIENT_INPUT"
"$CLIENT" fifo 20 < "$CLIENT_INPUT" > "$CLIENT_LOG" 2>&1 &
exec {CLIENT_FD}> "$CLIENT_INPUT"
wait_log '^mapped$'
"$UMBRIEL" settle > /dev/null

"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle > /dev/null

echo b >&"$CLIENT_FD"
wait_log '^discarded ' 19

echo "a hidden fifo-v1 surface keeps draining its barrier burst"
