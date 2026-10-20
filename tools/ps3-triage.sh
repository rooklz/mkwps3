#!/bin/sh
# ps3-triage.sh [files...] -- syntax-check runtime sources with the PS3 flags and
# print "<errors> <file> :: <first error>" per file (all runtime sources when no
# arguments). GEN is the translation workspace (translator output).
. "$(dirname "$0")/local.env" 2>/dev/null || true
GEN=${GEN:?set GEN in tools/local.env}
REPO=$(cd "$(dirname "$0")/.." && pwd)
FILES="$*"
docker run --rm -m 6g -v "$GEN":/ws -v "$REPO":/repo -w /repo/runtime ps3dev:latest sh -c '
G=/usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-g++
F="-std=gnu++17 -O1 -mcpu=cell -w -I/repo/platform/ps3/compat -include mkw_ps3_compat.h -I/repo/platform/ps3 -I/repo/runtime/include -I/repo/runtime/src -I/repo/runtime/src/hle -I/ws -I/ws/generated -I/repo/runtime/third_party/toml11 -I/repo/runtime/third_party/pugixml -I/usr/local/ps3dev/ppu/include -D__PS3__ -DTARGET_PC"
list="'"$FILES"'"
[ -z "$list" ] && list=$(find src -name "*.cpp" | sort)
for f in $list; do
  out=$($G $F -fsyntax-only $f 2>&1 | grep "error")
  n=$(printf "%s" "$out" | grep -c error)
  first=$(printf "%s\n" "$out" | head -1 | sed "s/^[^ ]*: //" | cut -c1-150)
  echo "$n $f :: $first"
done'
