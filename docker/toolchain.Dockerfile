# The PS3 cross toolchain: PPU and SPU GCC 7.2, binutils and PSL1GHT, built from
# ps3dev/ps3toolchain with every source pinned. It takes about an hour, so CI
# builds it once and publishes it (.github/workflows/images.yml) and the builder
# image starts from that.
#
#   docker build -f docker/toolchain.Dockerfile -t ghcr.io/rooklz/ps3-toolchain:<tag> .
FROM debian:12-slim

ARG PS3TOOLCHAIN_REV=e98d2ed5a5f58273ffce17d860f02af8f19eb9d9
ARG PSL1GHT_REV=f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe

ENV PS3DEV=/usr/local/ps3dev \
    PSL1GHT=/usr/local/ps3dev \
    PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/local/ps3dev/bin:/usr/local/ps3dev/ppu/bin:/usr/local/ps3dev/spu/bin \
    DEBIAN_FRONTEND=noninteractive

RUN apt-get -y update && apt-get -y install --no-install-recommends \
      autoconf automake bison build-essential ca-certificates cmake flex git \
      libelf-dev libgmp-dev libmpfr-dev libmpc-dev libncurses5-dev libssl-dev \
      libtool-bin pkg-config texinfo wget zlib1g-dev python3 make patch \
      xz-utils bzip2 file gettext rsync unzip \
    && rm -rf /var/lib/apt/lists/*

# ps3toolchain fetches PSL1GHT from its master branch; pin it like everything else.
RUN git clone https://github.com/ps3dev/ps3toolchain /build \
    && cd /build && git checkout "$PS3TOOLCHAIN_REV" \
    && sed -i "s|github.com/ps3dev/psl1ght/tarball/master|github.com/ps3dev/psl1ght/tarball/$PSL1GHT_REV|" archives/archives.txt \
    && grep -q "$PSL1GHT_REV" archives/archives.txt
WORKDIR /build

# One layer per stage, so a late failure doesn't throw away the compilers.
# Stage 9 (ps3libraries) isn't needed.
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 1
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 2
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 4
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 5
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 6
RUN MAKEFLAGS=-j$(nproc) ./toolchain.sh 8

RUN powerpc64-ps3-elf-gcc --version \
    && spu-gcc --version \
    && test -f /usr/local/ps3dev/ppu/include/rsx/nv40.h \
    && test -f /usr/local/ps3dev/ppu/lib/libgcm_sys.a \
    && test -x /usr/local/ps3dev/bin/pkg \
    && rm -rf /build

WORKDIR /work
