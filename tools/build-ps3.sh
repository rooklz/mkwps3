#!/bin/sh
# build-ps3.sh [make args...] -- build the PS3 ELF/SELF in the ps3dev image.
# GEN is the translation workspace (translator output and object directory).
. "$(dirname "$0")/local.env" 2>/dev/null || true
GEN=${GEN:?set GEN in tools/local.env}
REPO=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${JOBS:-6}
python3 "$REPO/tools/pack-sections.py" "$GEN/generated" "$GEN/build/ps3/mkwii_sections.bin" >/dev/null || exit 1
exec docker run --rm -m 7g -v "$GEN":/ws -v "$REPO":/repo -w /repo ps3dev:latest \
    make -f Makefile.ps3 -j"$JOBS" "$@"
