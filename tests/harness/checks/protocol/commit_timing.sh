#!/usr/bin/env bash
# commit-timing-v1: the global is advertised; a commit targeted 300 ms ahead is held until about its target and then
# presented, on its own, when a fifo-v1 barrier holds it as well, and when its wp_commit_timer_v1 is destroyed right
# after the commit (existing constraints stay in force); an out-of-range tv_nsec is invalid_timestamp, a
# second timestamp for one commit is timestamp_exists, and a second wp_commit_timer_v1 for one surface is
# commit_timer_exists.
set -euo pipefail

readonly CLIENT="$UMBRIEL_PACING_CLIENT"
CLIENT_PID=
CLIENT_LOG=
CLIENT_FD=

if [[ ! -x $CLIENT || ! -x $UMBRIEL_GLOBAL_CLIENT ]]; then
  echo "required harness clients are not built"
  exit 1
fi

"$UMBRIEL_GLOBAL_CLIENT" wp_commit_timing_manager_v1 present 1

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

# A configure arriving mid-burst would make the client commit its ack, superseding (discarding) the timed commit's
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

# assert_presented_near_target <label>: commit 0 was presented no earlier than half a refresh before its target. The
# half-period margin holds with or without the compositor's early-release tolerance (a quarter period), and a commit
# released as soon as it arrived would present about 300 ms early.
assert_presented_near_target() {
  local label=$1
  wait_log '^presented 0 '
  local refresh_mhz
  refresh_mhz=$(awk '$1 == "refresh-mhz" { print $2; exit }' "$CLIENT_LOG")
  # Headless outputs advertise no refresh rate; their frames run at 60 Hz.
  ((${refresh_mhz:-0} > 0)) || refresh_mhz=60000
  local half_period=$((1000000000000 / refresh_mhz / 2))
  if ! awk -v slack="$half_period" '
      $1 == "target" && $2 == 0 { target = $3 }
      $1 == "presented" && $2 == 0 { presented = $3 }
      END {
        if (target == "") { print "no target was logged"; exit 1 }
        if (presented < target - slack) {
          printf "presented at %d, %d ns before its target %d\n", presented, target - presented, target
          exit 1
        }
      }' "$CLIENT_LOG"; then
    echo "$label log: $(tr '\n' '|' < "$CLIENT_LOG")"
    exit 1
  fi
}

# A timed commit is held until about its target, then presented.
start_client timing timing 300
start_burst
assert_presented_near_target timing
stop_client

# A timed commit that a fifo barrier also holds: the barrier's release does not bypass the timestamp.
start_client timing-fifo timing-fifo 300
start_burst
assert_presented_near_target timing-fifo
stop_client

# Destroying the timer right after the timed commit leaves its constraint in force.
start_client timer-destroy-mid timer-destroy-mid 300
start_burst
assert_presented_near_target timer-destroy-mid
stop_client

# tv_nsec of a full second or more is invalid_timestamp.
start_client invalid timing-invalid
wait_log '^protocol-error wp_commit_timer_v1 0$'
stop_client

# A second timestamp before the commit is timestamp_exists.
start_client duplicate timing-duplicate
wait_log '^protocol-error wp_commit_timer_v1 1$'
stop_client

# A second wp_commit_timer_v1 for the same surface is commit_timer_exists.
start_client timer-duplicate timer-duplicate
wait_log '^protocol-error wp_commit_timing_manager_v1 0$'
stop_client

echo "commit-timing-v1 holds a timed commit until its target, with or without a fifo barrier or its timer, and rejects bad requests"
