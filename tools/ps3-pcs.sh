#!/bin/sh
# ps3-pcs.sh [secs] [top] -- sample the running app's guest and GPU threads and
# print a per-function profile.
. "$(dirname "$0")/local.env" 2>/dev/null || true
SECS=${1:-5}
TOP=${2:-25}
TMP=${TMPDIR:-/tmp}/mkw-pcs.$$
mkdir -p "$TMP"
curl -s -m 10 -Q "DELE /dev_hdd0/tmp/pcsamples.bin" "ftp://$PS3_IP:${PS3_FTP_PORT:-21}/" >/dev/null 2>&1
printf "pcs $SECS" | nc -w 3 "$PS3_IP" 4001
i=0
until curl -s -m 10 -o "$TMP/pcs.bin" "ftp://$PS3_IP:${PS3_FTP_PORT:-21}/dev_hdd0/tmp/pcsamples.bin" && [ -s "$TMP/pcs.bin" ]; do
    i=$((i + 1)); [ $i -gt $((SECS + 30)) ] && { echo "no samples"; exit 1; }
    sleep 1
done
docker run --rm -v "$GEN":/ws ps3dev:latest /usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm -n /ws/build/ps3/mkwii.elf > "$TMP/nm.txt"
MKW_MAP="$GEN/projects/mkwii/MAP.txt" python3 "$(dirname "$0")/pcs-report.py" "$TMP/pcs.bin" "$TMP/nm.txt" "$TOP"
rm -rf "$TMP"
