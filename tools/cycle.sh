#!/bin/sh
# cycle.sh [seconds] -- build, deploy, relaunch, wait, then print the log.
set -e
cd "$(dirname "$0")/.."
. tools/local.env 2>/dev/null || true
WAIT=${1:-40}
if ! tools/build-ps3.sh > "$GEN/build/build.log" 2>&1; then
    grep -E "error|Error" "$GEN/build/build.log" | head -20
    exit 1
fi
tools/deploy-ps3.sh
# The old process answers until it exits; wait for the new one to come up.
sleep 8
i=0
until [ "$(printf ping | nc -w 2 "$PS3_IP" 4001 2>/dev/null)" = "alive" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "app did not come back"; exit 1; }
    sleep 2
done
sleep "$WAIT"
tools/ps3-log.sh
