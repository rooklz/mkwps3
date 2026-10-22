#!/bin/sh
# bench-gp.sh [frames] -- the repeatable benchmark: relaunch, drive the menus
# to a 50cc Mushroom Cup Grand Prix (Luigi Circuit, 12 karts) with injected
# pad input, hand the player's kart to the game's CPU driver, and measure a
# fixed stretch of the race: race frames 600 to 600+frames (default 2400, about
# 40 seconds of game time). The random seeds are fixed for the run
# (mkwii-fixedseed.txt), so every run races the same race and the numbers only
# move when the code does.
#
# Prints fps, and game-thread and GPU-thread milliseconds per race frame.
# PCS=<secs> (PCS_TOP=<n>), GPROF=1 and HPROF=<gpu|game> also take a profile
# inside the measured stretch; see each below. A profile costs the run a little
# speed, so time a build without one.
. "$(dirname "$0")/local.env" 2>/dev/null || true
cd "$(dirname "$0")/.."
FRAMES=${1:-2400}
START=${START:-600}
FTP="ftp://$PS3_IP:${PS3_FTP_PORT:-21}"
ctl() { printf '%s' "$1" | nc -w 3 "$PS3_IP" 4001; }
press() { tools/ps3-pad.sh a 4 >/dev/null; sleep "$1"; }
# FIXED=0: let the game seed its random numbers from the clock as usual.
FIXED=${FIXED:-1}
if [ "$FIXED" = 1 ]; then
    printf 1 | curl -s -T - "$FTP/dev_hdd0/tmp/mkwii-fixedseed.txt"
    trap 'curl -s -Q "DELE /dev_hdd0/tmp/mkwii-fixedseed.txt" "$FTP/" >/dev/null 2>&1' EXIT
fi
ctl relaunch >/dev/null
sleep 12
i=0
until [ "$(ctl ping 2>/dev/null)" = "alive" ]; do
    i=$((i + 1)); [ $i -gt 30 ] && { echo "app did not come back"; exit 1; }
    sleep 2
done
sleep 25
# title movie, title, licence, Single Player, Grand Prix, 50cc, character,
# vehicle, drift, Mushroom Cup, OK
press 5; press 5; press 6; press 8; press 12; press 9; press 9; press 9; press 9; press 10
# Menus swallow a press now and then during transitions: keep confirming until
# the race scene exists, and not a press after it -- with the CPU driving the
# player's kart, a press in the race switches the spectator camera, and the
# run would no longer be the same race.
racing() {
    ctl mark | grep -q ' race 1$'
}
# AUTO=0: drive the player's kart by holding accelerate (it ends up in a wall)
# instead of handing it to the game's CPU driver like the other eleven.
AUTO=${AUTO:-1}
i=0
until racing; do
    i=$((i + 1)); [ $i -gt 20 ] && { echo "did not reach the race"; exit 1; }
    [ "$AUTO" = 1 ] && tools/ps3-autodrive.sh >/dev/null
    tools/ps3-pad.sh a 4 >/dev/null
    j=0
    while [ $j -lt 12 ] && ! racing; do sleep 0.5; j=$((j + 1)); done
done
[ "$AUTO" = 1 ] || tools/ps3-pad.sh a 6000 >/dev/null
# Wait for a race frame, polling the counters; prints the mark line.
mark_at() {
    i=0
    while :; do
        m=$(ctl mark)
        f=$(printf '%s' "$m" | sed -n 's/^frame \([0-9]*\) .*/\1/p')
        [ -n "$f" ] && [ "$f" -ge "$1" ] && { printf '%s\n' "$m"; return 0; }
        i=$((i + 1)); [ $i -gt 1200 ] && return 1
        sleep 0.25
    done
}
a=$(mark_at "$START") || { echo "the race never reached frame $START"; exit 1; }
# PCS=<seconds>: also take a guest profile inside the measured stretch.
[ -n "$PCS" ] && tools/ps3-pcs.sh "$PCS" "${PCS_TOP:-60}" > "${TMPDIR:-/tmp}/bench-pcs.txt" 2>&1
# GPROF=1: also the renderer's phase profile (control port `prof`) over the stretch.
[ -n "$GPROF" ] && ctl prof >/dev/null
# HPROF=<gpu|game> on a measurement build: also a native profile of that thread.
[ -n "$HPROF" ] && tools/ps3-hprof.sh "$HPROF" "${HPROF_SECS:-10}" "${HPROF_TOP:-60}" > "${TMPDIR:-/tmp}/bench-hprof.txt" 2>&1
# SEGMENT=<frames>: also report each stretch of that many frames.
marks=$a
f=$START
while [ "$f" -lt $((START + FRAMES)) ]; do
    f=$((f + ${SEGMENT:-FRAMES}))
    [ "$f" -gt $((START + FRAMES)) ] && f=$((START + FRAMES))
    m=$(mark_at "$f") || { echo "the race never reached frame $f"; exit 1; }
    marks="$marks
$m"
done
[ -n "$MARKS" ] && printf '%s\n' "$marks" >&2
printf '%s\n' "$marks" | python3 -c '
import sys
rows = [[int(x) for x in line.split()[1:9:2]] for line in sys.stdin.read().split("\n") if line]
def show(a, b, label):
    (f0, n0, i0, g0), (f1, n1, i1, g1) = a, b
    frames, wall = f1 - f0, (n1 - n0) / 1e6
    print(f"{label}race frames {f0}-{f1}: {frames / wall * 1000:.1f} fps, "
          f"game {(wall - (i1 - i0) / 1e6) / frames:.2f} ms/frame, gpu {(g1 - g0) / 1e6 / frames:.2f} ms/frame")
if len(rows) > 2:
    for a, b in zip(rows, rows[1:]):
        show(a, b, "  ")
show(rows[0], rows[-1], "")
'
[ -n "$GPROF" ] && ctl prof > "${TMPDIR:-/tmp}/bench-gprof.txt"
tools/ps3-shot.sh "${TMPDIR:-/tmp}/bench-gp.png" >/dev/null 2>&1
