#!/usr/bin/env bash
# A stationary SSD title bar press must not cancel the clicked tile's scrolling reveal when it repaints.
set -euo pipefail
source "$(dirname "$UMBRIEL_HARNESS_LIB")/title_bar_scroll.sh" forced
