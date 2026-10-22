#!/usr/bin/env bash
# The output listing reports whether a fullscreen window is scanned out directly, and why not when it is not. The state
# settles before it is reported, and each settled change is pushed to subscribers.
set -euo pipefail

readonly CLIENT="${UMBRIEL_SUBSURFACE_CLIENT:-./build-debug/tests/subsurface-client}"
readonly CLIENT_LOG="$UMBRIEL_RUNTIME_DIR/direct-scanout-client.log"
readonly EVENTS="$UMBRIEL_RUNTIME_DIR/direct-scanout-events.jsonl"

if [[ ! -x $CLIENT ]]; then
  echo "subsurface client not built at $CLIENT"
  exit 1
fi

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[animation]
duration_ms = 1
curve = "linear"
EOF
"$UMBRIEL" msg config-reload > /dev/null
"$UMBRIEL" subscribe outputs > "$EVENTS" &

# Waits until the newest pushed outputs event satisfies a jq predicate on HEADLESS-1's direct_scanout.
await_scanout() {
  local predicate=$1 reason=$2
  for _ in $(seq 60); do
    if jq -e -s "[.[] | select(.event == \"outputs\")] | last | .data[]
        | select(.name == \"HEADLESS-1\") | .direct_scanout | $predicate" "$EVENTS" > /dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  echo "$reason: the newest direct_scanout is $(jq -c -s '[.[] | select(.event == "outputs")] | last | .data[]
    | select(.name == "HEADLESS-1") | .direct_scanout' "$EVENTS" 2> /dev/null)"
  exit 1
}

await_scanout '.active == false and (.reason | type == "string") and .window == null' \
  "an empty output did not report inactive scanout with a reason"

# The opaque child subsurface covers its parent exactly, so the fullscreen window is one visible buffer.
env INITIAL_FULLSCREEN=1 OPAQUE_CONTENT=1 "$CLIENT" direct-scanout > "$CLIENT_LOG" 2>&1 &
for _ in $(seq 60); do
  grep -q '^mapped$' "$CLIENT_LOG" && break
  sleep 0.05
done
if ! grep -q '^mapped$' "$CLIENT_LOG"; then
  echo "fullscreen client never mapped: $(cat "$CLIENT_LOG")"
  exit 1
fi
window_id=$("$UMBRIEL" windows --json | jq -r '.[] | select(.title == "direct-scanout") | .id')

await_scanout ".active and .reason == null and .window.id == \"$window_id\" and .window.title == \"direct-scanout\"" \
  "a fullscreen window was not reported as scanned out"

# The query answers with the same settled state the stream carries.
if ! diff <("$UMBRIEL" outputs --json | jq -S .) \
  <(jq -s -S '[.[] | select(.event == "outputs")] | last | .data' "$EVENTS") > /dev/null; then
  echo "outputs --json differs from the pushed snapshot: $("$UMBRIEL" outputs --json)"
  exit 1
fi
if ! "$UMBRIEL" outputs | grep -q '^  Direct scanout: active (.* "direct-scanout")$'; then
  echo "the outputs listing does not show the scanned-out window: $("$UMBRIEL" outputs)"
  exit 1
fi

printf '\n[output.HEADLESS-1]\ndirect_scanout = false\n' >> "$UMBRIEL_CONFIG"
"$UMBRIEL" msg config-reload > /dev/null
await_scanout '.active == false and .reason == "disabled" and .window == null' \
  "turning direct scanout off for the output did not report it disabled"
if ! "$UMBRIEL" outputs | grep -q '^  Direct scanout: inactive (disabled)$'; then
  echo "the outputs listing does not show scanout disabled: $("$UMBRIEL" outputs)"
  exit 1
fi

if ! grep -q "output 'HEADLESS-1': direct scanout on (.* \"direct-scanout\")" "$UMBRIEL_LOG"; then
  echo "scanout starting was not logged"
  exit 1
fi
if ! grep -q "output 'HEADLESS-1': direct scanout off (disabled)" "$UMBRIEL_LOG"; then
  echo "scanout stopping was not logged"
  exit 1
fi
# Each lasting change is logged once.
if (($(grep -c "output 'HEADLESS-1': direct scanout o" "$UMBRIEL_LOG") != 2)); then
  echo "expected two direct scanout log lines: $(grep "direct scanout" "$UMBRIEL_LOG")"
  exit 1
fi
