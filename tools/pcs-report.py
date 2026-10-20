#!/usr/bin/env python3
"""pcs-report.py <pcsamples.bin> <nm -n output of mkwii.elf> [top]

Turns the control port's PC samples into a per-function profile for the guest
(game) and GPU threads. Translated functions are named func_XXXXXXXX after the
guest address they came from; the MAP.txt name is appended when given with
MKW_MAP=<path to MAP.txt>.
"""
import bisect
import os
import struct
import sys


def load_symbols(nm_path):
    addrs, names = [], []
    with open(nm_path) as f:
        for line in f:
            parts = line.split()
            if len(parts) < 3 or parts[1].lower() not in ("t", "w"):
                continue
            name = parts[2]
            if name.startswith("."):          # ELFv1 code entry symbols
                name = name[1:]
            addrs.append(int(parts[0], 16))
            names.append(name)
    order = sorted(range(len(addrs)), key=lambda i: addrs[i])
    return [addrs[i] for i in order], [names[i] for i in order]


def load_map(path):
    names = {}
    if not path or not os.path.exists(path):
        return names
    with open(path) as f:
        for line in f:
            parts = line.split(None, 1)
            if len(parts) == 2 and not parts[1].startswith("0x"):
                names[parts[0].upper()] = parts[1].strip()
    return names


def main():
    data = open(sys.argv[1], "rb").read()
    addrs, names = load_symbols(sys.argv[2])
    top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    guest_names = load_map(os.environ.get("MKW_MAP"))
    starts = sorted(int(k, 16) for k in guest_names)

    def guest_label(pc):
        i = bisect.bisect_right(starts, pc) - 1
        if i < 0:
            return f"{pc:08x}"
        start = starts[i]
        return f"{start:08x} {guest_names.get(f'{start:08X}', '')}"
    magic, variant, t0, t1, f0, f1, d0, d1 = struct.unpack_from(">8I", data, 0)
    off = 32
    guest_mode = magic in (0x50435333, 0x50435334)   # samples are guest addresses
    print(f"guest {t0} samples ({d0} dropped)")
    for thread in (("current function", "guest LR") if guest_mode else ("guest", "gpu")):
        (n,) = struct.unpack_from(">I", data, off)
        off += 4
        per_fn = {}
        total = 0
        for _ in range(n):
            pc, count = struct.unpack_from(">II", data, off)
            off += 8
            if guest_mode:
                name = guest_label(pc)
            else:
                i = bisect.bisect_right(addrs, pc) - 1
                name = names[i] if i >= 0 else f"?{pc:08x}"
            per_fn[name] = per_fn.get(name, 0) + count
            total += count
        print(f"\n== {thread} thread: {total} samples")
        for name, count in sorted(per_fn.items(), key=lambda kv: -kv[1])[:top]:
            label = name
            if name.startswith("func_") and len(name) >= 13:
                guest = guest_names.get(name[5:13].upper())
                if guest:
                    label += f"  [{guest}]"
            print(f"{100.0 * count / max(total, 1):6.2f}%  {count:7d}  {label}")
    if magic == 0x50435334 and off + 4 <= len(data):
        stack_report(data, off, guest_label, top)


def stack_report(data, off, guest_label, top):
    """Inclusive per-function time and a top-down tree from guest stack walks."""
    (used,) = struct.unpack_from(">I", data, off)
    off += 4
    end = off + used * 4
    samples = []
    while off < end:
        (n,) = struct.unpack_from(">I", data, off)
        off += 4
        words = struct.unpack_from(f">{n}I", data, off)
        off += 4 * n
        samples.append(words)
    idle = sum(1 for s in samples if s[0] == 1)
    busy = [s for s in samples if s[0] != 1]
    print(f"\n== stacks: {len(samples)} samples, {100.0 * idle / max(len(samples), 1):.1f}% idle")
    if not busy:
        return
    incl = {}
    paths = []
    for s in busy:
        # frames innermost first: current function, LR's function, then callers
        frames = [guest_label(s[0])] + [guest_label(a) for a in s[1:]]
        seen = set()
        for f in frames:
            if f not in seen:
                seen.add(f)
                incl[f] = incl.get(f, 0) + 1
        dedup = []
        for f in reversed(frames):
            if not dedup or dedup[-1] != f:
                dedup.append(f)
        paths.append(dedup)
    print(f"\n== inclusive (of {len(busy)} busy samples)")
    for name, count in sorted(incl.items(), key=lambda kv: -kv[1])[:top]:
        print(f"{100.0 * count / len(busy):6.2f}%  {name}")
    tree = {}
    for p in paths:
        node = tree
        for f in p[:10]:
            child = node.setdefault(f, [0, {}])
            child[0] += 1
            node = child[1]
    print("\n== top-down (>= 1.5% of busy)")
    limit = 0.015 * len(busy)

    def walk(node, depth):
        for name, (count, kids) in sorted(node.items(), key=lambda kv: -kv[1][0]):
            if count < limit:
                continue
            print(f"{'  ' * depth}{100.0 * count / len(busy):5.1f}%  {name}")
            walk(kids, depth + 1)
    walk(tree, 0)


if __name__ == "__main__":
    main()
