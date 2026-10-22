#!/usr/bin/env bash
# Transparent SSD grip bands resize a floating window with no modifier.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar.sh"
title_bar_config
env XDG_DECORATION=server "$UMBRIEL_FRACTIONAL_CLIENT" ssd-float-resize 480 300 > "$UMBRIEL_RUNTIME_DIR/resize.log" 2>&1 &
title_bar_wait_box ssd-float-resize 480x300+173+109
"$UMBRIEL" settle > /dev/null
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 170 250 press 272 move 130 250 release 272
title_bar_wait_box ssd-float-resize 520x300+133+109
"$UMBRIEL_POINTER_CLIENT" 1280 720 move 130 83 press 272 move 110 63 release 272
title_bar_wait_box ssd-float-resize 540x320+113+89
echo "floating SSD side and corner resize passed without a modifier"
