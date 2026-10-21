#!/bin/sh
# ps3-hprof.sh <gpu|game> [secs] [top] -- where a native thread spends its time,
# by function. Needs the measurement build on the console:
#
#   tools/build-ps3.sh OUT=/ws/build/ps3-hprof MKW_HPROF=1
#   PS3_OUT=$GEN/build/ps3-hprof tools/deploy-ps3.sh
#
# Translated game code isn't instrumented: on the game thread its time shows as
# "(translated code)"; tools/ps3-pcs.sh profiles that side.
. "$(dirname "$0")/local.env" 2>/dev/null || true
WHICH=${1:-gpu}
SECS=${2:-5}
TOP=${3:-30}
ELF=${HPROF_OUT:-$GEN/build/ps3-hprof}/mkwii.elf
TMP=${TMPDIR:-/tmp}/mkw-hprof.$$
mkdir -p "$TMP"
FTP="ftp://$PS3_IP:${PS3_FTP_PORT:-21}"
printf "hprof %s %s" "$WHICH" "$SECS" | nc -w $((SECS + 10)) "$PS3_IP" 4001
curl -s -m 30 -o "$TMP/hprof.bin" "$FTP/dev_hdd0/tmp/hprof.bin" || { echo "no samples"; exit 1; }
docker run --rm -v "$(dirname "$ELF")":/elf ps3dev:latest \
    /usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-nm -n /elf/mkwii.elf > "$TMP/nm.txt"
python3 - "$TMP/hprof.bin" "$TMP/nm.txt" "$TOP" <<'EOF'
import bisect, collections, struct, sys
data = open(sys.argv[1], "rb").read()
top = int(sys.argv[3])
syms = {}
starts = []
for line in open(sys.argv[2]):
    p = line.split()
    if len(p) == 3 and p[1] in "TtDdWw":
        a = int(p[0], 16)
        if a not in syms or syms[a].startswith("."):
            syms[a] = p[2].lstrip(".")
starts = sorted(syms)
def name(a):
    if a == 0:
        return "(translated code)"
    if a in syms:
        return syms[a]
    i = bisect.bisect_right(starts, a) - 1
    return syms[starts[i]] + "+?" if i >= 0 else hex(a)
pairs = [struct.unpack_from(">II", data, i) for i in range(0, len(data) - 7, 8)]
n = len(pairs)
own = collections.Counter(name(t) for t, _ in pairs)
edge = collections.Counter((name(c), name(t)) for t, c in pairs if t)
print(f"{n} samples")
print("\n== where it is (innermost instrumented function)")
for f, k in own.most_common(top):
    print(f"{100.0 * k / n:6.2f}%  {f}")
print("\n== called from")
for (c, f), k in edge.most_common(top):
    print(f"{100.0 * k / n:6.2f}%  {c} -> {f}")
EOF
rm -rf "$TMP"
