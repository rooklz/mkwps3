#!/bin/sh
# ps3-slow.sh [secs] -- which guest functions take the memory slow paths (a store
# into the GX pipe, MMIO, a quantized paired single, a range fallback), counted
# by the running app for a few seconds. An access the translator could have
# resolved at translation time shows up here with its function.
. "$(dirname "$0")/local.env" 2>/dev/null || true
SECS=${1:-10}
ctl() { printf '%s' "$1" | nc -w 5 "$PS3_IP" 4001; }
ctl "slow on" >/dev/null
sleep "$SECS"
ctl slow | python3 -c '
import bisect, sys
names = {}
for line in open(sys.argv[1]):
    parts = line.split(None, 1)
    if len(parts) == 2:
        try:
            names[int(parts[0], 16)] = parts[1].strip()
        except ValueError:
            pass
starts = sorted(names)
for line in sys.stdin:
    parts = line.split()
    if len(parts) < 3:
        continue
    i = bisect.bisect_right(starts, int(parts[0], 16)) - 1
    print(f"{line.rstrip():44} {names[starts[i]] if i >= 0 else ""}")
' "$(dirname "$0")/../upstream/wiicompiled/projects/mkwii/MAP.txt"
ctl "slow off" >/dev/null
