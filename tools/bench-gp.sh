#!/bin/sh
# bench-gp.sh [windows] -- the repeatable benchmark: relaunch, drive the menus
# to a 50cc Mushroom Cup Grand Prix (Luigi Circuit, 12 karts) with injected
# pad input, hold accelerate, and average the 5-second stats windows of the
# race: fps, game-thread and GPU-thread milliseconds per frame, calls per frame.
. "$(dirname "$0")/local.env" 2>/dev/null || true
cd "$(dirname "$0")/.."
N=${1:-6}
press() { tools/ps3-pad.sh a 4 >/dev/null; sleep "$1"; }
printf relaunch | nc -w 3 "$PS3_IP" 4001 >/dev/null
sleep 12
i=0
until [ "$(printf ping | nc -w 2 "$PS3_IP" 4001 2>/dev/null)" = "alive" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "app did not come back"; exit 1; }
    sleep 2
done
sleep 25
# title movie, title, licence, Single Player, Grand Prix, 50cc, character,
# vehicle, drift, Mushroom Cup, OK
press 5; press 5; press 6; press 8; press 12; press 9; press 9; press 9; press 9; press 10
# Menus swallow a press now and then during transitions: keep confirming until
# the stats show race-level work (the game thread at least 70% busy while the
# GX stream carries race-sized draw counts), then hold accelerate for the
# measurement. Neither needs the renderer, so this also works with
# /dev_hdd0/tmp/mkwii-norender.txt in place.
racing() {
    tools/ps3-log.sh | grep "^\[stats\]" | tail -1 |
        sed -n "s/.*busy -\{0,1\}\([0-9]*\)%.* draws \([0-9]*\) verts.*/\1 \2/p" |
        awk '{ ok = $1 >= 70 && $2 >= 800000 } END { exit !ok }'
}
i=0
until racing; do
    i=$((i + 1)); [ $i -gt 20 ] && { echo "did not reach the race"; exit 1; }
    press 6
done
sleep 60
tools/ps3-pad.sh a 6000 >/dev/null
# PCS=<seconds>: also take a guest profile inside the measured windows.
if [ -n "$PCS" ]; then
    sleep 5
    tools/ps3-pcs.sh "$PCS" 60 > "${TMPDIR:-/tmp}/bench-pcs.txt" 2>&1
    sleep $((N * 5 + 7 - PCS))
else
    sleep $((N * 5 + 12))
fi
tools/ps3-log.sh | grep "^\[stats\]" | tail -n "$N" | python3 -c '
import re, sys
rows = []
for line in sys.stdin:
    m = re.search(r"([\d.]+) fps, game thread busy (-?\d+)%.*?gpu (\d+) ms", line)
    if m:
        u = re.search(r"([\d.]+) us/call", line)
        rows.append(tuple(float(x) for x in m.groups()) + (float(u.group(1)) if u else 0.0,))
if not rows:
    sys.exit("no stats")
frames = sum(r[0] * 5 for r in rows)
game = sum(r[1] / 100 * 5000 for r in rows)
gpu = sum(r[2] for r in rows)
us = [r[3] for r in rows if r[3]]
line = f"{len(rows)} windows: {frames / (5 * len(rows)):.1f} fps, game {game / max(frames, 1):.1f} ms/frame, " \
       f"gpu {gpu / max(frames, 1):.1f} ms/frame, busy {game / (5000 * len(rows)) * 100:.0f}%"
if us:
    line += f", {sum(us) / len(us):.3f} us/call"
print(line)
'
tools/ps3-shot.sh "${TMPDIR:-/tmp}/bench-gp.png" >/dev/null 2>&1
