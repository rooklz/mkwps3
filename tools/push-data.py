#!/usr/bin/env python3
"""push-data.py <extracted DATA dir> -- sync the disc's files/ and sys/ to the
app's USRDIR/DATA over webMAN FTP. One listing per directory decides what is
missing or a different size; uploads retry on the flaky connections webMAN
gives a busy console. Settings come from tools/local.env."""
import ftplib
import os
import sys
import time


def env():
    vals = dict(os.environ)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "local.env")
    if os.path.exists(path):
        for line in open(path):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, v = line.split("=", 1)
                vals.setdefault(k, v)
    return vals


def connect(host, port):
    for attempt in range(10):
        try:
            ftp = ftplib.FTP(encoding="latin-1")   # webMAN listings are not UTF-8
            ftp.connect(host, port, timeout=60)
            ftp.login()
            return ftp
        except Exception as e:  # noqa: BLE001
            print(f"connect failed ({e}); retrying", flush=True)
            time.sleep(3)
    sys.exit("cannot connect")


def remote_sizes(ftp, path):
    sizes = {}
    lines = []
    for attempt in range(20):
        try:
            lines = []
            ftp.retrlines(f"LIST {path}", lines.append)
            break
        except ftplib.error_perm:
            return None
        except ftplib.error_temp as ex:
            # webMAN runs out of data sockets under a burst of transfers.
            print(f"busy ({ex}); waiting", flush=True)
            time.sleep(5)
    else:
        sys.exit(f"cannot list {path}")
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) == 9 and not line.startswith("d"):
            sizes[parts[8]] = int(parts[4])
    return sizes


def ensure_dir(ftp, path):
    cur = ""
    for part in path.strip("/").split("/"):
        cur += "/" + part
        try:
            ftp.mkd(cur)
        except ftplib.error_perm:
            pass


def main():
    src = sys.argv[1]
    e = env()
    host, port = e["PS3_IP"], int(e.get("PS3_FTP_PORT", "21"))
    root = f"/dev_hdd0/game/{e.get('PS3_APP', 'DOLPHIN01')}/USRDIR/DATA"
    ftp = connect(host, port)
    todo = []
    for top in ("sys", "files"):
        for dirpath, _, files in os.walk(os.path.join(src, top)):
            rel = os.path.relpath(dirpath, src)
            rdir = f"{root}/{rel}"
            have = remote_sizes(ftp, rdir)
            if have is None:
                ensure_dir(ftp, rdir)
                have = {}
            for name in sorted(files):
                local = os.path.join(dirpath, name)
                if have.get(name) != os.path.getsize(local):
                    todo.append((local, f"{rdir}/{name}"))
    total = sum(os.path.getsize(l) for l, _ in todo)
    print(f"{len(todo)} files to upload, {total >> 20} MiB", flush=True)
    done = 0
    for local, remote in todo:
        for attempt in range(6):
            try:
                with open(local, "rb") as f:
                    ftp.storbinary(f"STOR {remote}", f, blocksize=65536)
                break
            except ftplib.error_temp as ex:
                print(f"busy on {remote} ({ex}); waiting", flush=True)
                time.sleep(5)
            except Exception as ex:  # noqa: BLE001
                print(f"retry {remote}: {ex}", flush=True)
                try:
                    ftp.close()
                except Exception:  # noqa: BLE001
                    pass
                time.sleep(2)
                ftp = connect(host, port)
        else:
            print(f"FAILED {remote}", flush=True)
        done += 1
        if done % 50 == 0:
            print(f"{done}/{len(todo)}", flush=True)
    print("push complete", flush=True)


if __name__ == "__main__":
    main()
