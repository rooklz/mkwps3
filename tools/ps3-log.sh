#!/bin/sh
# ps3-log.sh [path] -- fetch the runtime log (or another file) from the console.
. "$(dirname "$0")/local.env" 2>/dev/null || true
PS3=${PS3_IP:?set PS3_IP in tools/local.env}
if [ -n "$1" ]; then
    curl -s -m 30 "ftp://$PS3:${PS3_FTP_PORT:-21}$1"
else
    curl -s -m 30 "ftp://$PS3:${PS3_FTP_PORT:-21}/dev_hdd0/tmp/mkwii-ps3.log"
    echo "---- stderr ----"
    curl -s -m 30 "ftp://$PS3:${PS3_FTP_PORT:-21}/dev_hdd0/tmp/mkwii-ps3.err"
fi
