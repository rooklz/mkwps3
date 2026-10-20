#!/bin/sh
# bench-race.sh [windows] -- relaunch, run the attract-mode race, and average
# the 5-second stats windows over the race: fps, game-thread and GPU-thread
# milliseconds per presented frame, translated calls per frame. Snapshots of
# the race vary with the camera; the average is what to compare between builds.
. "$(dirname "$0")/local.env" 2>/dev/null || true
cd "$(dirname "$0")/.."
N=${1:-6}
printf relaunch | nc -w 3 "$PS3_IP" 4001 >/dev/null
sleep 12
i=0
until [ "$(printf ping | nc -w 2 "$PS3_IP" 4001 2>/dev/null)" = "alive" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "app did not come back"; exit 1; }
    sleep 2
done
sleep 30
tools/ps3-pad.sh a 4 >/dev/null
sleep 12
sleep $((N * 5 + 2))
tools/ps3-log.sh | grep "^\[stats\]" | tail -n "$N" | python3 -c '
import re, sys
rows = []
for line in sys.stdin:
    m = re.search(r"([\d.]+) fps, game thread busy (-?\d+)%.*?calls (\d+)/frame.*?gpu (\d+) ms", line)
    if m:
        rows.append(tuple(float(x) for x in m.groups()))
if not rows:
    sys.exit("no stats")
frames = sum(r[0] * 5 for r in rows)
game = sum(r[1] / 100 * 5000 for r in rows)
gpu = sum(r[3] for r in rows)
calls = sum(r[2] * r[0] * 5 for r in rows)
print(f"{len(rows)} windows: {frames / (5 * len(rows)):.1f} fps, game {game / frames:.1f} ms/frame, "
      f"gpu {gpu / frames:.1f} ms/frame, {calls / frames:.0f} calls/frame")
'
