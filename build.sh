#!/bin/sh
# build.sh <your Mario Kart Wii disc image>
#
# Turns your own PAL disc image into an installable PS3 package, out/MKWP00001.pkg.
# Everything happens inside Docker, so that's the only thing you need installed.
# See docs/BUILDING.md if anything goes wrong.
set -eu
cd "$(dirname "$0")"

IMAGE=${MKWPS3_IMAGE:-ghcr.io/rooklz/mkwps3-builder:latest}
VOLUME=${MKWPS3_VOLUME:-mkwps3-work}

if [ $# -lt 1 ] || [ ! -f "$1" ]; then
    echo "usage: ./build.sh /path/to/your/MarioKartWii.wbfs"
    echo
    echo "Any Wii disc format works (ISO, WBFS, RVZ, WIA, CISO, GCZ). It has to be the"
    echo "PAL version, RMCP01, and a clean dump. The package lands in out/."
    exit 1
fi

command -v docker > /dev/null 2>&1 || {
    echo "Docker isn't installed. Grab Docker Desktop from https://www.docker.com/products/docker-desktop/"
    echo "(on Linux, your distro's docker package is fine), start it, then run this again."
    exit 1
}
docker info > /dev/null 2>&1 || {
    echo "Docker is installed but not running. Start Docker Desktop, give it a minute, then try again."
    exit 1
}

DISC_DIR=$(cd "$(dirname "$1")" && pwd)
DISC_NAME=$(basename "$1")

if ! docker image inspect "$IMAGE" > /dev/null 2>&1; then
    echo "Downloading the builder image (a couple of GB, only the first time)..."
    if ! docker pull "$IMAGE"; then
        echo
        echo "Couldn't download it, so building it here instead. This compiles the whole PS3"
        echo "toolchain and takes about an hour, but only ever happens once."
        docker build -f docker/toolchain.Dockerfile -t mkwps3-toolchain:local .
        docker build --build-arg TOOLCHAIN=mkwps3-toolchain:local -t "$IMAGE" .
    fi
fi

mkdir -p out
docker volume create "$VOLUME" > /dev/null

TTY=
[ -t 1 ] && TTY=-t
# Your checkout is mounted read-only over the copy in the image, so local changes get built.
# The workspace (extracted disc, translated code, objects) lives in a Docker volume, which
# keeps rebuilds quick: ./build.sh again only redoes what changed.
exec docker run --rm $TTY \
    -v "$DISC_DIR/$DISC_NAME:/game/$DISC_NAME:ro" \
    -v "$(pwd)/out:/out" \
    -v "$(pwd):/repo:ro" \
    -v "$VOLUME:/ws" \
    -e JOBS="${JOBS:-}" \
    "$IMAGE" "/game/$DISC_NAME" /out
