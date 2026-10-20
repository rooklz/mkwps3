#!/bin/sh
# ps3-pad.sh <button...> [reads] -- press GameCube buttons on controller 1.
#   buttons: a b x y start z l r up down left right (combine with +), reads default 8
. "$(dirname "$0")/local.env" 2>/dev/null || true
mask=0
for b in $(echo "$1" | tr '+' ' '); do
    case $b in
        left) v=0x0001;; right) v=0x0002;; down) v=0x0004;; up) v=0x0008;;
        z) v=0x0010;; r) v=0x0020;; l) v=0x0040;; a) v=0x0100;; b) v=0x0200;;
        x) v=0x0400;; y) v=0x0800;; start) v=0x1000;; *) echo "unknown button $b"; exit 1;;
    esac
    mask=$((mask | v))
done
printf "pad %x %d %s" "$mask" "${2:-8}" "${3:-}" | nc -w 3 "$PS3_IP" 4001
