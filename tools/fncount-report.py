#!/usr/bin/env python3
"""fncount-report.py <fncount.bin> <generated/functions dir> [frames] [top]

From `FNCOUNT=1 tools/bench-gp.sh` on a build made with EXTRA_GEN_FLAGS=-DMKW_COUNT_CALLS=2.

Entries per translated function over a bench stretch, joined with the shape of each
function's translated body: guest instructions, blocks, calls out, loops. Groups the
call volume by why a call to it is still a call (leaf too big / too many blocks / loop /
makes calls)."""
import collections, os, re, struct, sys

data = open(sys.argv[1], "rb").read()
gen = sys.argv[2]
frames = int(sys.argv[3]) if len(sys.argv) > 3 else 2400
top = int(sys.argv[4]) if len(sys.argv) > 4 else 60
counts = {}
for i in range(0, len(data) - 7, 8):
    a, c = struct.unpack_from(">II", data, i)
    counts[a] = counts.get(a, 0) + c
total = sum(counts.values())

insn_re = re.compile(r"MKW_GUEST_INSNS\((\d+)\)")
label_re = re.compile(r"^loc_([0-9A-F]{8}):", re.M)
call_re = re.compile(r"InvokeDirectCpu<0x([0-9A-F]+)u>|InvokeIndirect|func_([0-9A-F]{8})\(ctx\)")
goto_re = re.compile(r"goto loc_([0-9A-F]{8});")


def shape(addr):
    p = os.path.join(gen, "func_%08X.cpp" % addr)
    if not os.path.exists(p):
        return None
    s = open(p).read()
    insns = sum(int(x) for x in insn_re.findall(s)) // 2 or 1   # MKW_GUEST_INSNS runs about two per instruction
    labels = [int(x, 16) for x in label_re.findall(s)]
    calls = [m for m in call_re.findall(s)]
    back = any(int(t, 16) <= int(l, 16) for l, t in
               ((l, t) for l, body in zip(label_re.findall(s), re.split(r"^loc_[0-9A-F]{8}:", s, flags=re.M)[1:])
                for t in goto_re.findall(body)))
    return dict(insns=insns, blocks=len(labels), calls=len(calls), loop=back)


rows = []
cats = collections.Counter()
for a, c in sorted(counts.items(), key=lambda kv: -kv[1]):
    sh = shape(a)
    if sh is None:
        cat = "native/other"
    elif sh["calls"] == 0 and sh["loop"]:
        cat = "leaf with loop"
    elif sh["calls"] == 0 and sh["insns"] > 56:
        cat = "leaf > 56 insns"
    elif sh["calls"] == 0 and sh["blocks"] > 8:
        cat = "leaf, > 8 blocks"
    elif sh["calls"] == 0:
        cat = "leaf (inlinable shape)"
    elif sh["insns"] <= 40:
        cat = "non-leaf <= 40 insns"
    else:
        cat = "non-leaf larger"
    cats[cat] += c
    rows.append((a, c, sh, cat))

print(f"{total} entries, {total / frames:.0f} per frame, {len(counts)} functions")
print("\n== call volume by shape of the callee")
for cat, c in cats.most_common():
    print(f"{100.0 * c / total:6.2f}%  {c / frames:8.0f}/frame  {cat}")
print(f"\n== top {top}")
for a, c, sh, cat in rows[:top]:
    desc = "-" if sh is None else f"{sh['insns']:4d} insns {sh['blocks']:3d} blocks {sh['calls']:2d} calls{' loop' if sh['loop'] else ''}"
    print(f"{a:08X} {c / frames:8.1f}/frame  {desc}  [{cat}]")
