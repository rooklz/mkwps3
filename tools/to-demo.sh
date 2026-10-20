#!/bin/sh
# to-demo.sh -- from a fresh launch, get to the attract-mode demo race (12 karts)
# and print the GPU phase profile and stats once it has run for a while.
. "$(dirname "$0")/local.env" 2>/dev/null || true
cd "$(dirname "$0")/.."
sleep 30
tools/ps3-pad.sh a 4 >/dev/null
sleep ${1:-40}
printf prof | nc -w 3 "$PS3_IP" 4001
printf gfx | nc -w 3 "$PS3_IP" 4001
tools/ps3-log.sh | grep "^\[stats\]" | tail -2
