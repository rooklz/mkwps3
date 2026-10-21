#!/usr/bin/env python3
"""ftrace.py <ftrace.bin> [--draws] [--skin] -- decode the renderer's one-frame trace
(control port `ftrace`): every EFB copy, and every draw with the textures it
samples and the copy targets backing them. By default only copies and draws
that sample a copy are printed."""
import struct
import sys

REC = struct.Struct(">BBBBHHI8I6f2I2I2h4I")


def main():
    data = open(sys.argv[1], "rb").read()
    magic, count, size, _ = struct.unpack_from(">4I", data, 0)
    if magic != 0x46545231 or size != REC.size:
        sys.exit(f"not an ftrace file (record size {size}, expected {REC.size})")
    show_all = "--draws" in sys.argv
    draws = 0
    for i in range(count):
        r = REC.unpack_from(data, 16 + i * size)
        kind, prim, ortho, tev, nverts, ind, idx = r[0:7]
        bp = r[7:15]
        tex, texwh, copy, extra = r[21:23], r[23:25], r[25:27], r[27:31]
        if kind == 2:
            sx, sy = extra[0] >> 16, extra[0] & 0xFFFF
            w, h = extra[1] >> 16, extra[1] & 0xFFFF
            f = extra[3]
            print(f"@{idx:5d} COPY src {sx},{sy} {w}x{h} -> {extra[2]:08x} fmt {f & 0xF:x}"
                  f"{' half' if f >> 30 & 1 else ''}{' xfb' if f >> 29 & 1 else ''}"
                  f"{' clear' if f >> 31 else ''}{' I' if f >> 28 & 1 else ''} pe {bp[2] & 7}")
        else:
            draws += 1
            uses_copy = any(c >= 0 for c in copy)
            if show_all or uses_copy or ("--skin" in sys.argv and ortho & 2):
                t = "  ".join(f"{a:08x} {wh >> 16}x{wh & 0xFFFF}{f' [copy{c}]' if c >= 0 else ''}"
                              for a, wh, c in zip(tex, texwh, copy) if a)
                print(f"@{idx:5d} draw {'ortho' if ortho & 1 else 'persp'}{' skin' if ortho & 2 else ''} {nverts:5d}v tev {tev} ind {ind}  {t}")
    print(f"{count} records, {draws} draws")


if __name__ == "__main__":
    main()
