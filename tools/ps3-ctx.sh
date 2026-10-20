#!/bin/sh
# ps3-ctx.sh [guest|gpu] -- dump a thread's registers from the running app and
# symbolise pc/lr against the matching unstripped ELF.
. "$(dirname "$0")/local.env" 2>/dev/null || true
PS3=${PS3_IP:?set PS3_IP in tools/local.env}
GEN=${GEN:?set GEN in tools/local.env}
OUT=$(printf 'ctx %s' "${1:-guest}" | nc -w 5 "$PS3" 4001)
echo "$OUT"
PC=$(echo "$OUT" | sed -n 's/^pc=\([0-9a-f]*\) lr=\([0-9a-f]*\).*/\1 \2/p')
[ -z "$PC" ] && exit 0
docker run --rm -v "$GEN":/ws ps3dev:latest sh -c "
  for a in $PC; do /usr/local/ps3dev/ppu/bin/powerpc64-ps3-elf-addr2line -f -C -e /ws/build/ps3/mkwii.elf 0x\$a | head -1; done"
