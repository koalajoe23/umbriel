#!/usr/bin/env bash
# fifo-v1 with commit-timing-v1 as Mesa's and NVIDIA's Vulkan WSI drive them in FIFO mode: each frame is a timed commit
# that sets and waits for the barrier, followed by an empty commit that only waits. Visible, every frame presents, each
# on its own refresh and never well before its target. Hidden, every frame's feedback resolves, the last one included:
# the trailing empty commit is the later content update that discards it, which is what keeps vkWaitForPresentKHR from
# blocking forever on an occluded window (Mesa's wsi_common_wayland.c).
set -euo pipefail

readonly CLIENT="$UMBRIEL_PACING_CLIENT"
readonly COUNT=20
CLIENT_PID=
CLIENT_LOG=
CLIENT_FD=

if [[ ! -x $CLIENT ]]; then
  echo "pacing-client is not built"
  exit 1
fi

# start_client <label>: runs the pacing client in wsi-fifo mode with its stdin on a pipe held open in CLIENT_FD.
start_client() {
  local label=$1
  local input=$UMBRIEL_RUNTIME_DIR/$label.in
  CLIENT_LOG=$UMBRIEL_RUNTIME_DIR/$label.log
  rm -f "$input"
  mkfifo "$input"
  "$CLIENT" wsi-fifo "$COUNT" < "$input" > "$CLIENT_LOG" 2>&1 &
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

stop_client() {
  exec {CLIENT_FD}>&-
  kill -TERM "$CLIENT_PID" 2>/dev/null || true
  wait "$CLIENT_PID" 2>/dev/null || true
}

half_period() {
  local refresh_mhz
  refresh_mhz=$(awk '$1 == "refresh-mhz" { print $2; exit }' "$CLIENT_LOG")
  # Headless outputs advertise no refresh rate; their frames run at 60 Hz.
  ((${refresh_mhz:-0} > 0)) || refresh_mhz=60000
  echo $((1000000000000 / refresh_mhz / 2))
}

# Visible: every frame presents, a refresh apart, and none more than half a period before its target. A configure
# arriving mid-burst would make the client commit its ack and discard a frame, so the burst starts once settled.
start_client visible
wait_log '^mapped$'
"$UMBRIEL" settle > /dev/null
echo b >&"$CLIENT_FD"
wait_log '^done$'
if ! awk -v gap="$(half_period)" -v count="$COUNT" '
    $1 == "target" { target[$2] = $3 }
    $1 == "presented" { at[$2] = $3 }
    $1 == "discarded" { printf "frame %d was discarded\n", $2; bad = 1 }
    END {
      for (i = 0; i < count; i++) {
        if (!(i in at)) { printf "frame %d was not presented\n", i; exit 1 }
        if (at[i] < target[i] - gap) {
          printf "frame %d presented %d ns before its target\n", i, target[i] - at[i]
          exit 1
        }
        if (i > 0 && at[i] - at[i - 1] < gap) {
          printf "frames %d and %d presented %d ns apart, under %d\n", i - 1, i, at[i] - at[i - 1], gap
          exit 1
        }
      }
      exit bad
    }' "$CLIENT_LOG"; then
  echo "visible log: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi
stop_client

# Hidden: nothing presents, so every frame resolves as discarded, the last one through its trailing empty commit.
start_client hidden
wait_log '^mapped$'
"$UMBRIEL" settle > /dev/null
"$UMBRIEL" msg workspace-switch:2 > /dev/null
"$UMBRIEL" settle > /dev/null
echo b >&"$CLIENT_FD"
wait_log '^done$'
if ! awk -v count="$COUNT" '
    $1 == "discarded" { seen[$2] = 1 }
    END {
      for (i = 0; i < count; i++) {
        if (!seen[i]) { printf "frame %d was not discarded\n", i; exit 1 }
      }
    }' "$CLIENT_LOG"; then
  echo "hidden log: $(tr '\n' '|' < "$CLIENT_LOG")"
  exit 1
fi
stop_client

echo "Vulkan WSI frame pairs present one per refresh when visible and all resolve when hidden"
