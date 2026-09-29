#!/usr/bin/env bash
# fifo-v1 on a hidden surface: with the window's workspace switched away there are no frame callbacks to clear the
# barrier, so the hidden-surface tick must keep a barrier burst draining, one commit per tick. Each commit a later one
# supersedes resolves as discarded while nothing shows it, so the discards arrive a tick apart; without holding, all
# 20 commits would apply at once and the 19 discards would arrive together.
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

# The hidden tick runs at the last known pacing output's rate: the headless output, which advertises no refresh rate
# and runs at 60 Hz. The 18 tick gaps between discards 0 and 18 must add up to at least 9 half-ticks (an honest drain
# spans about 18 ticks, an unheld burst microseconds). The discard time is when the client reads the event, so a floor
# on the total span, rather than on each gap, absorbs a client stall that delivers several discards together.
refresh_mhz=$(awk '$1 == "refresh-mhz" { print $2; exit }' "$CLIENT_LOG")
((${refresh_mhz:-0} > 0)) || refresh_mhz=60000
min_gap=$((1000000000000 / refresh_mhz / 2))
if ! awk -v gap="$min_gap" '
    $1 == "discarded" { at[$2] = $3; seen[$2] = 1 }
    END {
      for (i = 0; i < 19; i++) {
        if (!seen[i]) { printf "commit %d was not discarded\n", i; exit 1 }
      }
      span = at[18] - at[0]
      if (span < 9 * gap) {
        printf "discards 0 to 18 span %d ns, under the %d ns floor\n", span, 9 * gap
        exit 1
      }
    }' "$CLIENT_LOG"; then
  echo "hidden burst log: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi

echo "a hidden fifo-v1 surface keeps draining its barrier burst"
