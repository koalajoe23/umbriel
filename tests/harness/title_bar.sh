# Shared fixture for the SSD title bar checks, using real decoration negotiation.
source "$UMBRIEL_HARNESS_LIB"

title_bar_config() {
  cat >> "$UMBRIEL_CONFIG" <<'EOF'
[animation]
enabled = false
[appearance]
border_width = 0
outer_border_width = 0
corner_radius = 0
[appearance.shadow]
enabled = false
[appearance.title_bar]
mode = "on"
height = 28
[colors.title_bar]
background = "#FF00FFFF"
background_unfocused = "#FF00FFFF"
close = "#00FF00FF"
[[window_rule]]
match.title = "^ssd-float"
default_floating = true
default_floating_size_px = { width = 480, height = 300 }
default_position = { x = 173, y = 109, anchor = "top_left" }
EOF
  "$UMBRIEL" msg config-reload > /dev/null
}

title_bar_window() {
  "$UMBRIEL" windows --json | jq -c --arg title "$1" '.[] | select(.title == $title)'
}

title_bar_wait_box() {
  local title=$1 expected=$2 actual=
  for _ in $(seq 120); do
    actual=$(title_bar_window "$title" | jq -r '"\(.w)x\(.h)+\(.x)+\(.y)"')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.025
  done
  echo "$title: expected $expected, got $actual"
  return 1
}

title_bar_wait_field() {
  local title=$1 field=$2 expected=$3 actual=
  for _ in $(seq 120); do
    actual=$(title_bar_window "$title" | jq -r --arg field "$field" '.[$field]')
    [[ $actual == "$expected" ]] && return 0
    sleep 0.025
  done
  echo "$title: expected $field=$expected, got $actual"
  return 1
}

title_bar_wait_absent() {
  for _ in $(seq 120); do
    [[ -z $(title_bar_window "$1") ]] && return 0
    sleep 0.025
  done
  echo "$1: window did not close"
  return 1
}
