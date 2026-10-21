#!/bin/sh
# ps3-autodrive.sh -- hand the player's kart to the game's own CPU driver for the
# next race, so benchmarks drive whole laps the way the other eleven karts do
# instead of holding accelerate into a wall.
#
# It sets player 1's type to CPU in the menu scenario, which the game copies
# into the race when the race loads, so run it on the last menu screen before
# confirming (running it again is harmless). The HUD and camera stay on the
# player's kart. PAL only: RaceConfig::spInstance is at 0x809BD728, its menu
# scenario at +0xC10, the players from +8 at 0xF0 apart, the type at +0x10.
. "$(dirname "$0")/local.env" 2>/dev/null || true
FTP="ftp://$PS3_IP:${PS3_FTP_PORT:-21}"
ctl() { printf '%s' "$1" | nc -w 3 "$PS3_IP" 4001; }

ctl "peek 809bd728 4" >/dev/null
inst=$(curl -s -m 10 "$FTP/dev_hdd0/tmp/peek.bin" | od -An -tx1 | tr -d ' \n')
case "$inst" in
    8*|9*) ;;
    *) echo "no race config yet ($inst)"; exit 1 ;;
esac
ctl "poke $(printf '%x' $((0x$inst + 0xC28))) 1" >/dev/null
