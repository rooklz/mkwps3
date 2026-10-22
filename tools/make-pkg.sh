#!/bin/sh
# make-pkg.sh <disc image> <output dir>
#
# The whole build in one go, meant to run inside the builder image (see
# Dockerfile and build.sh): check and extract the disc, translate the game,
# compile it for the PS3 and pack the lot into an installable .pkg.
#
# Everything in between lives in the workspace ($WS). Each step leaves a stamp
# there, so running it again only redoes what's changed.
set -eu

DISC=${1:?usage: make-pkg.sh <disc image> <output dir>}
OUT_DIR=${2:?usage: make-pkg.sh <disc image> <output dir>}
REPO=${REPO:-/repo}
WS=${WS:-/ws}
APPID=${APPID:-MKWP00001}
TITLE=${TITLE:-Mario Kart Wii}
SUPPORT=${SUPPORT:-/opt/mkwps3}     # dsp_coef.bin, wii_bootstrap/ and pkg-key (see Dockerfile)
CONTENTID=UP0001-${APPID}_00-0000000000000000
PS3_BIN=${PS3DEV:-/usr/local/ps3dev}/bin

step() { printf '\n==> %s\n' "$*"; }
fail() { printf '\nerror: %s\n' "$*" >&2; exit 1; }
elapsed() { t=$(( $(date +%s) - START )); printf '%dm%02ds' $((t / 60)) $((t % 60)); }
START=$(date +%s)

if [ -z "${JOBS:-}" ]; then
    # Each compiler job on the big translated files wants up to ~1.5 GB.
    cores=$(nproc)
    mem_gb=$(awk '/MemTotal/ { print int($2 / 1048576) }' /proc/meminfo)
    JOBS=$(( mem_gb * 2 / 3 ))
    [ "$JOBS" -gt "$cores" ] && JOBS=$cores
    [ "$JOBS" -lt 1 ] && JOBS=1
fi

[ -f "$DISC" ] || fail "can't find a disc image at $DISC"
mkdir -p "$WS" "$OUT_DIR"

# --- 1. The disc ------------------------------------------------------------
# Only the clean PAL release works: the translation is pinned to its exact
# main.dol and StaticR.rel.
step "Checking the disc"
ID=$(nodtool info "$DISC" 2>/dev/null | sed -n 's/^Game ID: *\([A-Z0-9]\{6\}\).*/\1/p' | head -1)
[ -n "$ID" ] || fail "nodtool couldn't read $DISC. Is it a Wii disc image (ISO, WBFS, RVZ, ...)?"
[ "$ID" = RMCP01 ] || fail "this is $ID, but only the PAL version (RMCP01) is supported for now"
echo "Mario Kart Wii, PAL (RMCP01)"

DISC_KEY="$(basename "$DISC") $(wc -c < "$DISC")"
if [ ! -f "$WS/disc/sys/main.dol" ] || [ "$(cat "$WS/.disc" 2>/dev/null)" != "$DISC_KEY" ]; then
    step "Extracting the disc (a few minutes)"
    rm -rf "$WS/disc" "$WS/.disc"
    nodtool extract -q "$DISC" "$WS/disc"
    printf '%s' "$DISC_KEY" > "$WS/.disc"
fi

mkdir -p "$WS/Assets" "$WS/projects"
cp "$WS/disc/sys/main.dol" "$WS/disc/files/rel/StaticR.rel" "$WS/Assets/"
check() {
    want=$(sed -n "/^  $1:/,/sha256/s/.*sha256: *//p" "$REPO/upstream/wiicompiled/projects/mkwii/recomp.yml")
    got=$(sha256sum "$WS/Assets/$2" | cut -d' ' -f1)
    [ "$got" = "$want" ] || fail "$2 doesn't match the clean PAL release. Modded or patched discs won't work, dump a clean one."
}
check dol main.dol
check rel StaticR.rel

# --- 2. The translator -------------------------------------------------------
TR_SRC="$REPO/upstream/wiicompiled/translator"
TR_KEY=$(cd "$TR_SRC" && find src -type f \( -name '*.cs' -o -name '*.csproj' \) -not -path '*/bin/*' -not -path '*/obj/*' \
    | LC_ALL=C sort | xargs cat | sha256sum | cut -d' ' -f1)
TR_DLL="$WS/translator/bin/Translator.Cli.dll"
if [ ! -f "$TR_DLL" ] || [ "$(cat "$WS/translator/.key" 2>/dev/null)" != "$TR_KEY" ]; then
    step "Building the translator"
    rm -rf "$WS/translator"
    mkdir -p "$WS/translator"
    # Built from a copy so the repository (mounted read-only) stays clean.
    cp -r "$TR_SRC" "$WS/translator/src"
    dotnet build "$WS/translator/src/src/Translator.Cli/Translator.Cli.csproj" -c Release -o "$WS/translator/bin" \
        -nologo -v quiet -p:TreatWarningsAsErrors=false > "$WS/translator/build.log" 2>&1 \
        || { tail -30 "$WS/translator/build.log"; fail "the translator didn't build"; }
    printf '%s' "$TR_KEY" > "$WS/translator/.key"
fi

# --- 3. Translation ------------------------------------------------------------
# PowerPC to C++, about thirty thousand functions. Redone when the translator,
# the project or the runtime's native bindings change.
rm -rf "$WS/projects/mkwii"
cp -r "$REPO/upstream/wiicompiled/projects/mkwii" "$WS/projects/mkwii"
ln -sfn "$REPO/runtime" "$WS/runtime"
GEN_KEY=$( { echo "$TR_KEY"; cat "$WS/projects/mkwii/recomp.yml"; \
    find "$REPO/runtime/src" "$REPO/runtime/include" -type f | LC_ALL=C sort | xargs cat; } | sha256sum | cut -d' ' -f1)
if [ ! -d "$WS/build/shards/base_common" ] || [ "$(cat "$WS/.translated" 2>/dev/null)" != "$GEN_KEY" ]; then
    step "Translating the game (10-20 minutes)"
    rm -f "$WS/.translated"
    rm -rf "$WS/generated" "$WS/build/shards" "$WS/build/base"
    P=projects/mkwii/recomp.yml
    THREADS=${THREADS:-$(nproc)}
    cd "$WS"
    dotnet "$TR_DLL" translate-recursive 0x800060A4 --project $P --outdir generated/functions \
        --output-metadata build/base/translation_metadata.json --threads "$THREADS" > translate.log 2>&1 \
        || { tail -30 translate.log; fail "translation failed (full log: translate.log in the workspace)"; }
    echo "  $(ls generated/functions | wc -l) functions"
    dotnet "$TR_DLL" generate-data-init --project $P > datainit.log 2>&1 \
        || { tail -30 datainit.log; fail "generating the data initialisers failed"; }
    dotnet "$TR_DLL" emit-build-shards --project $P --base-metadata build/base/translation_metadata.json \
        --base-functions-dir generated/functions --native-source-dir runtime/src --out build/shards > shards.log 2>&1 \
        || { tail -30 shards.log; fail "emitting the build shards failed"; }
    printf '%s' "$GEN_KEY" > "$WS/.translated"
    echo "  done after $(elapsed)"
fi

# --- 4. Compile ----------------------------------------------------------------
step "Compiling for the PS3 with $JOBS jobs (30-90 minutes the first time, much less after)"
OUT=$WS/build/ps3
mkdir -p "$OUT"
python3 "$REPO/tools/pack-sections.py" "$WS/generated" "$OUT/mkwii_sections.bin" > /dev/null
make -s -f "$REPO/Makefile.ps3" -j"$JOBS" REPO="$REPO" WS="$WS" > "$WS/compile.log" 2>&1 \
    || { grep -E "error|Error" "$WS/compile.log" | head -20; fail "compiling failed (full log: compile.log in the workspace)"; }
echo "  done after $(elapsed)"

# --- 5. Package ----------------------------------------------------------------
step "Packing $APPID.pkg (the game files go in too, so this takes a few minutes)"
PKG_DIR=$WS/pkg
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/USRDIR"
cp "$REPO/pkg/ICON0.PNG" "$REPO/pkg/PIC1.PNG" "$PKG_DIR/"
"$PS3_BIN/sfo" --title "$TITLE" --appid "$APPID" -f "$REPO/pkg/sfo.xml" "$PKG_DIR/PARAM.SFO" > /dev/null
cp "$OUT/EBOOT.BIN" "$OUT/RELOAD.SELF" "$OUT/vtx_spu.elf" "$OUT/ax_spu.elf" "$OUT/gxf_spu.elf" "$OUT/mkwii_sections.bin" "$PKG_DIR/USRDIR/"
cp "$SUPPORT/dsp_coef.bin" "$PKG_DIR/USRDIR/"
cp -r "$SUPPORT/wii_bootstrap" "$PKG_DIR/USRDIR/"
mkdir -p "$PKG_DIR/USRDIR/DATA"
# Hard links where the filesystem allows it: the game files are ~2.5 GB.
cp -rl "$WS/disc/files" "$WS/disc/sys" "$PKG_DIR/USRDIR/DATA/" 2>/dev/null \
    || cp -r "$WS/disc/files" "$WS/disc/sys" "$PKG_DIR/USRDIR/DATA/"

# PSL1GHT's pkg tool tops out at 1024 files and builds the package in memory, so this
# uses a streaming one that writes the same format.
python3 "$REPO/tools/mkpkg.py" "$CONTENTID" "$PKG_DIR" "$WS/$APPID.pkg" --key-file "$SUPPORT/pkg-key" > /dev/null
mv "$WS/$APPID.pkg" "$OUT_DIR/$APPID.pkg"
# The container runs as root; give the package to whoever owns the output folder.
chown "$(stat -c %u:%g "$OUT_DIR")" "$OUT_DIR/$APPID.pkg" 2>/dev/null || true
rm -rf "$PKG_DIR"

step "All done in $(elapsed)"
echo "Your package: $(basename "$OUT_DIR")/$APPID.pkg ($(du -h "$OUT_DIR/$APPID.pkg" | cut -f1))"
echo "It has your copy of the game inside it, so keep it to yourself."
