#!/usr/bin/env bash
# commit-timing-v1 in steady state: a stream of timed commits, each committed as soon as the previous one presented,
# never presents earlier than half a refresh period before its target. One stream spaces its targets a period apart,
# so after the first they have passed by the time they arrive; the other keeps each target a period past the previous
# present, at a phase moving by a quarter period per commit, so release decisions extrapolate from fresh presents.
set -euo pipefail

readonly CLIENT="$UMBRIEL_PACING_CLIENT"
readonly COUNT=40
CLIENT_LOG=

if [[ ! -x $CLIENT ]]; then
  echo "pacing-client is not built"
  exit 1
fi

# wait_log <pattern>: polls the client log until `pattern` matches a line.
wait_log() {
  local pattern=$1
  for _ in $(seq 400); do
    grep -q -- "$pattern" "$CLIENT_LOG" && return 0
    sleep 0.025
  done
  echo "timed out waiting for '$pattern': $(tr '\n' '|' < "$CLIENT_LOG")"
  return 1
}

# run_stream <label> [lead-periods]: streams COUNT timed commits, the first 100 ms ahead, and asserts that every one
# was presented no earlier than half a period before its target.
run_stream() {
  local label=$1
  shift
  local input=$UMBRIEL_RUNTIME_DIR/$label.in
  CLIENT_LOG=$UMBRIEL_RUNTIME_DIR/$label.log
  mkfifo "$input"
  "$CLIENT" timing-stream "$COUNT" 100 "$@" < "$input" > "$CLIENT_LOG" 2>&1 &
  local pid=$!
  local fd
  exec {fd}> "$input"
  # A configure arriving mid-stream would make the client commit its ack and discard a timed commit's feedback.
  wait_log '^mapped$'
  "$UMBRIEL" settle > /dev/null
  echo b >&"$fd"
  wait_log '^done$'
  exec {fd}>&-
  kill -TERM "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true

  local refresh_mhz
  refresh_mhz=$(awk '$1 == "refresh-mhz" { print $2; exit }' "$CLIENT_LOG")
  # Headless outputs advertise no refresh rate; their frames run at 60 Hz.
  ((${refresh_mhz:-0} > 0)) || refresh_mhz=60000
  if ! awk -v slack="$((1000000000000 / refresh_mhz / 2))" -v count="$COUNT" '
      $1 == "target" { target[$2] = $3 }
      $1 == "presented" { presented[$2] = $3 }
      END {
        bad = 0
        for (i = 0; i < count; i++) {
          if (!(i in presented)) { printf "commit %d was not presented\n", i; bad = 1; continue }
          if (presented[i] < target[i] - slack) {
            printf "commit %d presented %d ns before its target\n", i, target[i] - presented[i]
            bad = 1
          }
        }
        exit bad
      }' "$CLIENT_LOG"; then
    echo "$label log: $(tr '\n' '|' < "$CLIENT_LOG")"
    exit 1
  fi
}

run_stream spaced
run_stream lead 1

echo "a stream of timed commits never presents more than half a period before its targets"
