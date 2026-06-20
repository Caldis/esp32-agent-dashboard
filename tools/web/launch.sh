#!/usr/bin/env bash
# launch.sh — one-command start for the web dev panel (real device + mirror).
#
#   ./launch.sh            # drive COM9, observe mode, web on :8090
#   ./launch.sh COM5       # different serial port
#   ./launch.sh --mock     # no hardware: mock TCP device
#
# Then open http://127.0.0.1:8090/
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "${1:-}" = "--mock" ]; then
    echo "[launch] mock mode — web on http://127.0.0.1:8090/"
    exec python "$here/serve.py" --spawn-bridge
else
    serial="${1:-COM9}"
    echo "[launch] real device on $serial — web on http://127.0.0.1:8090/"
    exec python "$here/serve.py" --spawn-bridge --serial "$serial" --auto off
fi
