#!/usr/bin/env bash
# Gate a private compositor's environment import and observe its real readiness
# socket before allowing startup applications to run. No host services are used.
set -euo pipefail
python3 "$UMBRIEL_REPO/tests/session_readiness.py" "$UMBRIEL"
