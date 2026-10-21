#!/usr/bin/env python3
"""mkpkg.py <content id> <package root> <out.pkg> --key-file <file>

Builds an installable PS3 package, in the same format PSL1GHT's `pkg` followed by
`package_finalize` produce, but streamed: that pair caps a package at 1024 files and
holds the whole thing in memory twice, and Mario Kart Wii is 2000-odd files and 2.5 GB.

The finalised package's data is AES-128-CTR encrypted under the PS3's public homebrew
package key, which isn't kept in this repository: the builder image takes it from
PSL1GHT's sources (see Dockerfile) and passes it in with --key-file. The encryption
itself runs through the openssl command-line tool.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import threading

TYPE_NPDRMSELF = 0x1
TYPE_RAW = 0x3
TYPE_DIRECTORY = 0x4
TYPE_OVERWRITE_ALLOWED = 0x80000000
PKG_MAGIC = 0x7F504B47
SELF_MAGIC = b"SCE\x00\x00\x00\x00\x02\x80"
CHUNK = 8 << 20


class Entry:
    def __init__(self, name, src, size, flags):
        self.name = name.encode()
        self.src = src
        self.size = size
        self.flags = flags
        self.name_off = 0
        self.data_off = 0

    @property
    def is_dir(self):
        return (self.flags & 0xFF) == TYPE_DIRECTORY


def collect(root):
    """Same order as PSL1GHT's pkg: a folder's files (sorted), then each subfolder
    (sorted), its own entry first and then its contents."""
    entries = []

    def walk(folder):
        names = sorted(n for n in os.listdir(folder) if not n.startswith("."))
        paths = [os.path.join(folder, n) for n in names]
        files = sorted(p for p in paths if not os.path.isdir(p))
        dirs = sorted(p for p in paths if os.path.isdir(p))
        for path in files:
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            size = os.path.getsize(path)
            if rel == "USRDIR/EBOOT.BIN":
                entries.append(Entry(rel, path, ((size - 0x30 + 63) & ~63) + 0x30,
                                     TYPE_OVERWRITE_ALLOWED | TYPE_NPDRMSELF))
            else:
                entries.append(Entry(rel, path, size, TYPE_OVERWRITE_ALLOWED | TYPE_RAW))
        for path in dirs:
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            entries.append(Entry(rel, None, 0, TYPE_OVERWRITE_ALLOWED | TYPE_DIRECTORY))
            walk(path)

    walk(root)
    return entries


def key_context(key):
    return bytearray(key[0:8] + key[0:8] + key[8:16] + key[8:16] + bytes(32))


def sha1_stream(ctx, data, counter=0):
    """The debug packages' stream cipher: SHA-1 of a 64-byte context per 16 bytes."""
    out = bytearray(len(data))
    for off in range(0, len(data), 16):
        ctx[0x38:0x40] = struct.pack(">Q", (counter + off // 16) & 0xFFFFFFFFFFFFFFFF)
        block = hashlib.sha1(bytes(ctx)).digest()
        for i in range(min(16, len(data) - off)):
            out[off + i] = block[i] ^ data[off + i]
    return bytes(out)


def eboot_with_meta(data, content_id):
    """An NPDRM SELF gets its NPD metadata written over the 0x80 bytes after its type-3
    digest block, as `pkg` does. Anything else goes in untouched."""
    if len(data) < 0x68 or data[:9] != SELF_MAGIC:
        return data
    app_info_off = struct.unpack_from(">Q", data, 40)[0]
    digest_off = struct.unpack_from(">Q", data, 88)[0]
    app_type = struct.unpack_from(">I", data, app_info_off + 12)[0] if app_info_off + 0x18 <= len(data) else 0
    found = False
    doff = digest_off
    while doff + 0x10 <= len(data):
        dtype, dsize = struct.unpack_from(">II", data, doff)
        is_next = struct.unpack_from(">Q", data, doff + 8)[0]
        if dtype == 3:
            found = True
            break
        doff += dsize
        if is_next != 1:
            break
    doff += 0x10
    if app_type != 8 or not found:
        return data
    sha = hashlib.sha1(data).digest()
    meta = bytearray(0x80)
    struct.pack_into(">IIII", meta, 0, 0x4E504400, 1, 3, 1)
    cid = content_id.encode()[:0x30]
    meta[16:16 + len(cid)] = cid
    for j in range(16):
        meta[0x40 + j] = sha[j]
        meta[0x50 + j] = (~sha[j]) & 0xFF
        meta[0x60 + j] = ((1 if j == 15 else 0) ^ meta[0x50 + j] ^ 0xAA) & 0xFF
    return data[:doff] + bytes(meta) + data[doff + 0x80:]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("content_id")
    ap.add_argument("root")
    ap.add_argument("out")
    ap.add_argument("--key-file", required=True, help="hex file with the 16-byte package key")
    args = ap.parse_args()
    key = bytes.fromhex(open(args.key_file).read().strip())
    if len(key) != 16:
        sys.exit("the key file should hold 32 hex digits")

    entries = collect(args.root)
    count = len(entries)

    # Layout: 0x20-byte descriptors, then 16-aligned names, then 16-aligned file data.
    cur = 0x20 * count
    for e in entries:
        e.name_off = cur
        cur += (len(e.name) + 15) & ~15
    for e in entries:
        e.data_off = cur
        if not e.is_dir:
            cur += (e.size + 15) & ~15
    data_size = cur
    if data_size >= 1 << 32:
        sys.exit("package data over 4 GB isn't supported by the format")

    desc = bytearray()
    for e in entries:
        desc += struct.pack(">IIQQII", e.name_off, len(e.name), e.data_off, e.size, e.flags, 0)
    for e in entries:
        desc += e.name + bytes(((len(e.name) + 15) & ~15) - len(e.name))

    hdr = bytearray(0x80)
    struct.pack_into(">IIIIIIQQQ", hdr, 0, PKG_MAGIC, 1, 0xC0, 5, 0x80, count,
                     data_size + 0x1A0, 0x140, data_size)
    cid = args.content_id.encode()[:0x30]
    hdr[0x30:0x30 + len(cid)] = cid

    # QA digest: every file's original bytes, a few header fields, then the descriptors.
    qa = hashlib.sha1()
    for e in entries:
        if not e.is_dir:
            with open(e.src, "rb") as f:
                for chunk in iter(lambda: f.read(CHUNK), b""):
                    qa.update(chunk)
    qa.update(hdr[0:0x30])
    qa.update(hdr[0x60:0x80])
    qa.update(hdr[0x60:0x80])
    qa.update(hdr[0x60:0x70])
    qa.update(desc)
    digest = qa.digest()[:16]
    hdr[0x60:0x70] = digest
    hdr[0x70:0x80] = sha1_stream(key_context(digest), bytes(16), 0xFFFFFFFFFFFFFFFF)

    meta = struct.pack(">11I2H3I2H", 1, 4, 3, 2, 4, 5, 3, 4, 0x0E, 4, 8, 0, 0,
                       data_size, 5, 4, 0x1061, 0)
    hdr_sha = hashlib.sha1(hdr).digest()
    meta_sha = hashlib.sha1(meta).digest()[3:19]
    enc1 = sha1_stream(key_context(meta_sha), bytes(0x30))
    enc2 = sha1_stream(key_context(hdr_sha[3:19]), enc1)

    hdr[4] = 0x80  # finalised
    head = bytes(hdr) + hdr_sha[3:19] + enc2 + meta + meta_sha + enc1
    assert len(head) == 0x140

    whole = hashlib.sha1()
    tmp = args.out + ".part"
    with open(tmp, "wb") as out:
        out.write(head)
        whole.update(head)
        aes = subprocess.Popen(
            # package_finalize's counter starts from the licensee block at 0x70.
            ["openssl", "enc", "-aes-128-ctr", "-nosalt", "-K", key.hex(), "-iv", bytes(hdr[0x70:0x80]).hex()],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE)

        def feed():
            try:
                aes.stdin.write(bytes(desc))
                for e in entries:
                    if e.is_dir:
                        continue
                    if e.flags & 0xFF == TYPE_NPDRMSELF:
                        with open(e.src, "rb") as f:
                            data = eboot_with_meta(f.read(), args.content_id)
                        aes.stdin.write(data)
                        written = len(data)
                    else:
                        written = 0
                        with open(e.src, "rb") as f:
                            for chunk in iter(lambda: f.read(CHUNK), b""):
                                aes.stdin.write(chunk)
                                written += len(chunk)
                    aes.stdin.write(bytes(((e.size + 15) & ~15) - written))
            finally:
                aes.stdin.close()

        feeder = threading.Thread(target=feed)
        feeder.start()
        total = 0
        for chunk in iter(lambda: aes.stdout.read(CHUNK), b""):
            out.write(chunk)
            whole.update(chunk)
            total += len(chunk)
        feeder.join()
        if aes.wait() != 0 or total != data_size:
            sys.exit(f"encrypting the package data failed ({total} of {data_size} bytes)")

        trailer = bytes(0x40)
        whole.update(trailer)
        out.write(trailer + whole.digest() + bytes(12))
    os.replace(tmp, args.out)
    print(f"{args.out}: {count} entries, {data_size + 0x1A0} bytes")


if __name__ == "__main__":
    main()
