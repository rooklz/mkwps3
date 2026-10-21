#!/bin/sh
# to-race.sh -- relaunch and drive the menus into a 50cc Grand Prix race (Luigi
# Circuit), then hold accelerate (HOLD=0: sit on the grid instead, for a steady
# view of the karts; AUTO=1: let the game's CPU driver race the player's kart). Returns once the stats show a race running, for
# debugging sessions that need the console sat in a race (bench-gp.sh does the same
# and then measures).
. "$(dirname "$0")/local.env" 2>/dev/null || true
cd "$(dirname "$0")/.."
press() { tools/ps3-pad.sh a 4 >/dev/null; sleep "$1"; }
racing() {
    tools/ps3-log.sh | grep "^\[stats\]" | tail -1 |
        sed -n "s/.*busy -\{0,1\}\([0-9]*\)%.* draws \([0-9]*\) verts.*/\1 \2/p" |
        awk '{ ok = $1 >= 70 && $2 >= 800000 } END { exit !ok }'
}
printf relaunch | nc -w 3 "$PS3_IP" 4001 >/dev/null
sleep 12
i=0
until [ "$(printf ping | nc -w 2 "$PS3_IP" 4001 2>/dev/null)" = "alive" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "app did not come back"; exit 1; }
    sleep 2
done
sleep 25
press 5; press 5; press 6; press 8; press 12; press 9; press 9; press 9; press 9; press 10
# AUTO=1: the game's CPU driver takes the player's kart (tools/ps3-autodrive.sh).
i=0
until racing; do
    i=$((i + 1)); [ $i -gt 20 ] && { echo "did not reach the race"; exit 1; }
    [ "${AUTO:-0}" = 1 ] && tools/ps3-autodrive.sh >/dev/null
    press 6
done
[ "${HOLD:-1}" = 1 ] && [ "${AUTO:-0}" != 1 ] && tools/ps3-pad.sh a 20000 >/dev/null
echo racing
