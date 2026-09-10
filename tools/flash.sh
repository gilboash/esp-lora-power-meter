#!/usr/bin/env bash
# Flash a board without stopping the dashboard.
#
# The server holds the serial port, which an upload needs exclusively. Killing
# the server for each flash works but is easy to forget to undo -- so instead it
# is asked to release its ports for a moment, and resumes on its own afterwards.
#
#   tools/flash.sh meter_node sense
#   tools/flash.sh meter_gateway gateway
set -euo pipefail

ENV=${1:?usage: flash.sh <pio-env> <sense|gateway>}
BOARD=${2:?usage: flash.sh <pio-env> <sense|gateway>}
API=${WALKTEST_API:-http://localhost:8420}

PORT=$(python3 "$(dirname "$0")/find_port.py" "$BOARD")
echo "flashing $ENV -> $BOARD at $PORT"

# Best-effort: the dashboard may not be running, which is fine.
curl -s -m 2 -X POST -H 'Content-Type: application/json' \
     -d '{"seconds":120}' "$API/api/pause" >/dev/null 2>&1 || true

resume() { curl -s -m 2 -X POST "$API/api/resume" >/dev/null 2>&1 || true; }
trap resume EXIT

sleep 1
pio run -e "$ENV" -t upload --upload-port "$PORT"
echo "done; dashboard resuming"
