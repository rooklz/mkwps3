#!/bin/sh
# deploy-ps3.sh -- push the built EBOOT.BIN/RELOAD.SELF into the app's USRDIR
# over webMAN FTP, then ask the running app (control port 4001) to relaunch.
# If nothing answers on 4001 the app is not running: start it from the XMB.
. "$(dirname "$0")/local.env" 2>/dev/null || true
set -e
GEN=${GEN:?set GEN in tools/local.env}
PS3=${PS3_IP:?set PS3_IP in tools/local.env}
PORT=${PS3_FTP_PORT:-21}
APP=${PS3_APP:-MKWP00001}
OUT=${PS3_OUT:-$GEN/build/ps3}
DEST="ftp://$PS3:$PORT/dev_hdd0/game/$APP/USRDIR"
# The SPU programs are loaded from USRDIR at run time; a stale one looks like a hung SPU.
# A build in another OUT (a measurement build) shares the main build's data
# sections, which build-ps3.sh always writes to $GEN/build/ps3.
for f in mkwii_sections.bin vtx_spu.elf ax_spu.elf RELOAD.SELF EBOOT.BIN; do
    src=$OUT/$f
    [ -f "$src" ] || src=$GEN/build/ps3/$f
    curl -s -m 1800 -T "$src" "$DEST/$f"
    echo "  pushed $f ($(( $(wc -c < "$src") / 1024 )) KiB)"
done
# Runtime assets the HLE loads from beside the executable. They are not part
# of the repository (see README): put them in local/ or point PS3_ASSETS at them.
REPO=$(cd "$(dirname "$0")/.." && pwd)
ASSETS=${PS3_ASSETS:-$REPO/local}
if [ "${PUSH_ASSETS:-1}" = 1 ]; then
    if [ -f "$ASSETS/dsp_coef.bin" ]; then
        curl -s -m 60 -T "$ASSETS/dsp_coef.bin" "$DEST/dsp_coef.bin"
    else
        echo "  no $ASSETS/dsp_coef.bin; skipped"
    fi
    # First-run NAND payload (WC24 files), seeded into a new managed NAND.
    if [ -d "$ASSETS/wii/shared2" ]; then
        (cd "$ASSETS/wii" && find shared2 -type f) | while read -r f; do
            curl -s -m 60 --ftp-create-dirs -T "$ASSETS/wii/$f" "$DEST/wii_bootstrap/$f"
        done
    fi
fi
# RELAUNCH=0 only replaces the files (for a console that must be restarted by
# hand first, e.g. after a GPU hang).
if [ "${RELAUNCH:-1}" != 1 ]; then
    echo "  not relaunching"
elif printf 'relaunch' | nc -w 3 "$PS3" 4001 2>/dev/null | grep -q spawning; then
    echo "  relaunched"
else
    echo "  app not running: start it from the XMB"
fi
