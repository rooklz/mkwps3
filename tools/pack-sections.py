#!/usr/bin/env python3
"""pack-sections.py <generated dir> <out file>

Packs the translator's DOL/REL section blobs into one file the PS3 runtime
reads straight into guest memory at boot (platform/ps3/data_sections_ps3.cpp),
instead of embedding them in the EBOOT where they would occupy RAM twice.

Format (big-endian): "MKWS", count, then count x {guest addr, size, offset},
then the data.
"""
import os
import re
import struct
import sys

gen, out = sys.argv[1], sys.argv[2]
src = open(os.path.join(gen, "data_sections_init.cpp")).read()
sizes = dict(re.findall(r"constexpr size_t kSize_(\w+) = (\d+)u;", src))
entries = []
for addr, name in re.findall(r"Memory::Contains\(0x([0-9A-Fa-f]+)u, kSize_(\w+)\)", src):
    blob = os.path.join(gen, "data_sections_init_blobs", name + ".bin")
    data = open(blob, "rb").read()
    if len(data) != int(sizes[name]):
        sys.exit(f"{name}: blob is {len(data)} bytes, initializer expects {sizes[name]}")
    entries.append((int(addr, 16), data))
header = struct.pack(">4sI", b"MKWS", len(entries))
offset = len(header) + 12 * len(entries)
table = b""
for addr, data in entries:
    table += struct.pack(">III", addr, len(data), offset)
    offset += len(data)
with open(out, "wb") as f:
    f.write(header + table + b"".join(d for _, d in entries))
print(f"packed {len(entries)} sections, {offset} bytes -> {out}")
