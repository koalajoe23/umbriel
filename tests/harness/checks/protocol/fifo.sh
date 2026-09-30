#!/usr/bin/env bash
# fifo-v1: the global is advertised; a burst of barrier commits presents one commit per refresh with none dropped; a
# second wp_fifo_v1 for one surface is already_exists; destroying the fifo mid-burst releases every held commit; and a
# client killed with commits held leaves the compositor able to settle.
set -euo pipefail

readonly CLIENT="$UMBRIEL_PACING_CLIENT"
CLIENT_PID=
CLIENT_LOG=
CLIENT_FD=

if [[ ! -x $CLIENT || ! -x $UMBRIEL_GLOBAL_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

"$UMBRIEL_GLOBAL_CLIENT" wp_fifo_manager_v1 present 1

# start_client <label> <mode> [args]: runs the pacing client with its stdin on a pipe held open in CLIENT_FD.
start_client() {
  local label=$1
  shift
  local input=$UMBRIEL_RUNTIME_DIR/$label.in
  CLIENT_LOG=$UMBRIEL_RUNTIME_DIR/$label.log
  rm -f "$input"
  mkfifo "$input"
  "$CLIENT" "$@" < "$input" > "$CLIENT_LOG" 2>&1 &
  CLIENT_PID=$!
  exec {CLIENT_FD}> "$input"
}

# wait_log <pattern> [count]: polls the client log until `pattern` matches at least `count` lines.
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

# A configure arriving mid-burst would make the client commit its ack, superseding (discarding) a burst commit's
# presentation feedback, so bursts start only once the window's configures are all acknowledged.
start_burst() {
  wait_log '^mapped$'
  "$UMBRIEL" settle > /dev/null
  echo b >&"$CLIENT_FD"
}

stop_client() {
  exec {CLIENT_FD}>&-
  kill -TERM "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

# A burst of 20 presents every commit, each on a later refresh than the one before.
start_client burst fifo 20
start_burst
wait_log '^done$'
presented=$(grep -c '^presented ' "$CLIENT_LOG" || true)
discarded=$(grep -c '^discarded ' "$CLIENT_LOG" || true)
if ((presented != 20 || discarded != 0)); then
  echo "expected 20 presented and 0 discarded, got $presented and $discarded: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi
refresh_mhz=$(awk '$1 == "refresh-mhz" { print $2; exit }' "$CLIENT_LOG")
# Headless outputs advertise no refresh rate; their frames run at 60 Hz.
((${refresh_mhz:-0} > 0)) || refresh_mhz=60000
min_gap=$((1000000000000 / refresh_mhz / 2))
if ! awk -v gap="$min_gap" '
    $1 == "presented" { at[$2] = $3; seen[$2] = 1 }
    END {
      for (i = 0; i < 20; i++) {
        if (!seen[i]) { printf "commit %d was not presented\n", i; exit 1 }
        if (i > 0 && at[i] - at[i - 1] < gap) {
          printf "commits %d and %d presented %d ns apart, under %d\n", i - 1, i, at[i] - at[i - 1], gap
          exit 1
        }
      }
    }' "$CLIENT_LOG"; then
  echo "burst log: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi
stop_client

# A second wp_fifo_v1 for the same surface is a protocol error.
start_client duplicate fifo-duplicate
wait_log '^protocol-error wp_fifo_manager_v1 0$'
stop_client

# Destroying the fifo object mid-burst releases the held commits, and a later commit still resolves.
start_client destroy fifo-destroy-mid 20
start_burst
wait_log '^done$'
stop_client

# A client that dies with commits held must not wedge the compositor.
start_client kill fifo 200
start_burst
wait_log '^presented '
kill -KILL "$CLIENT_PID"
wait "$CLIENT_PID" 2>/dev/null || true
exec {CLIENT_FD}>&-
"$UMBRIEL" settle > /dev/null

echo "fifo-v1 paces barrier bursts one per refresh, rejects duplicates, and survives destroy and client death"
