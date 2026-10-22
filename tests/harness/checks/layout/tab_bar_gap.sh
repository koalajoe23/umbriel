#!/usr/bin/env bash
# The tab bar's gap sets the space between it and its window on every edge, and -1 keeps the layout gap.
set -euo pipefail

readonly CLIENT="${UMBRIEL_UNMAP_CLIENT:-./build-debug/tests/unmap-client}"
readonly FRAME="$UMBRIEL_RUNTIME_DIR/frame.png"

cat >> "$UMBRIEL_CONFIG" <<'EOF'

[layout]
gap = 24

[layout.tabs]
default_display = "tabbed"

[appearance]
border_width = 0
outer_border_width = 0

[appearance.tab_bar]
style = "indicator"
position = "top"
gap = -1

[colors.tab_bar]
active = "#FF0000FF"
EOF
"$UMBRIEL" msg config-reload > /dev/null

FILL_COLOR=0xFF00FF00 RESIZE_FILL_COLOR=0xFF00FF00 "$CLIENT" tab 300 200 > /dev/null 2>&1 &
for _ in $(seq 80); do
  [[ $("$UMBRIEL" windows --json | jq length) == 1 ]] && break
  sleep 0.05
done

# Prints the bar's box, then the tab's.
boxes() {
  "$UMBRIEL" settle
  grim "$FRAME"
  echo "$("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'r > 0.9 && g < 0.1') $("$UMBRIEL_PIXEL_PROBE" "$FRAME" bbox 'g > 0.9 && r < 0.1')"
}

# With its bar hidden, the tab fills the column the bar, its gap and the tab share.
"$UMBRIEL" msg column-hide-tab-bar > /dev/null
read -r _ _ _ _ cx cy cw ch <<< "$(boxes)"
"$UMBRIEL" msg column-show-tab-bar > /dev/null

expect() {
  sed -i "s/^position = .*/position = \"$1\"/; /^\[appearance\.tab_bar\]/,/^\[/ s/^gap = .*/gap = $2/" "$UMBRIEL_CONFIG"
  "$UMBRIEL" msg config-reload > /dev/null
  local bx by bw bh x y w h ok
  read -r bx by bw bh x y w h <<< "$(boxes)"
  # The bar sits on the column's edge, the tab reaches the opposite one, and $3 separates them.
  case $1 in
    top) ok=$((by == cy && y + h == cy + ch && y - by - bh == $3 && bw == w)) ;;
    bottom) ok=$((y == cy && by + bh == cy + ch && by - y - h == $3 && bw == w)) ;;
    left) ok=$((bx == cx && x + w == cx + cw && x - bx - bw == $3 && bh == h)) ;;
    right) ok=$((x == cx && bx + bw == cx + cw && bx - x - w == $3 && bh == h)) ;;
  esac
  if ((!ok)); then
    echo "$1 bar, gap $2: expected a $3 gap in $cx,$cy ${cw}x$ch, got bar $bx,$by ${bw}x$bh, tab $x,$y ${w}x$h"
    exit 1
  fi
}

for position in top bottom left right; do
  expect "$position" 4 4
done
expect top -1 24

echo "the tab bar's gap spaced it from its window on every edge, and -1 kept the layout gap"
