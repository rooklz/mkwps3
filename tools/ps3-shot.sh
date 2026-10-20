#!/bin/sh
# ps3-shot.sh [out.png] -- grab the next presented frame from the running app.
. "$(dirname "$0")/local.env" 2>/dev/null || true
OUT=${1:-${TMPDIR:-/tmp}/mkwii-shot.png}
FTP="ftp://$PS3_IP:${PS3_FTP_PORT:-21}"
curl -s -m 10 -Q "DELE /dev_hdd0/tmp/mkwii-shot.ppm" "$FTP/" >/dev/null 2>&1
printf shot | nc -w 3 "$PS3_IP" 4001 >/dev/null
sleep 2
curl -s -m 30 -o "$OUT.ppm" "$FTP/dev_hdd0/tmp/mkwii-shot.ppm" || { echo "no screenshot"; exit 1; }
sips -s format png "$OUT.ppm" --out "$OUT" >/dev/null 2>&1 && rm -f "$OUT.ppm"
echo "$OUT"
