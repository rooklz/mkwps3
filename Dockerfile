# The MKW PS3 builder: the PS3 toolchain plus everything else the build needs
# (.NET for the translator, nodtool for reading discs, Dolphin's two freely
# licensed data files), so Docker is the only thing you have to install.
# build.sh and build.ps1 drive it; tools/make-pkg.sh is what runs inside.
ARG TOOLCHAIN=ghcr.io/rooklz/ps3-toolchain:gcc7.2-psl1ght-f649a08
FROM ${TOOLCHAIN}

ARG NODTOOL_VERSION=v1.4.4
ARG DOLPHIN_REV=cf5cb7ef81c3e8e7af5c9d566334219ffda9f68b
ARG PSL1GHT_REV=f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe

ENV DOTNET_ROOT=/usr/share/dotnet \
    DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1 \
    DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 \
    DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1 \
    PATH=$PATH:/usr/share/dotnet

RUN apt-get -y update && apt-get -y install --no-install-recommends curl ca-certificates python3 make openssl \
    && rm -rf /var/lib/apt/lists/*

RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 8.0 --install-dir /usr/share/dotnet \
    && rm /tmp/dotnet-install.sh \
    && dotnet --info > /dev/null

RUN case "$(uname -m)" in \
        x86_64) arch=x86_64 ;; \
        aarch64|arm64) arch=aarch64 ;; \
        *) echo "no nodtool build for $(uname -m)"; exit 1 ;; \
    esac \
    && curl -fsSL "https://github.com/encounter/nod-rs/releases/download/$NODTOOL_VERSION/nodtool-linux-$arch" -o /usr/local/bin/nodtool \
    && chmod +x /usr/local/bin/nodtool \
    && nodtool --version

# Dolphin's free replacement DSP coefficient ROM and its default WiiConnect24
# tree. The runtime loads both; neither comes from Nintendo.
RUN base="https://raw.githubusercontent.com/dolphin-emu/dolphin/$DOLPHIN_REV/Data/Sys" \
    && mkdir -p /opt/mkwps3/wii_bootstrap/shared2/wc24/mbox \
    && curl -fsSL "$base/GC/dsp_coef.bin" -o /opt/mkwps3/dsp_coef.bin \
    && for f in misc.bin nwc24dl.bin nwc24fl.bin nwc24fls.bin nwc24msg.cbk nwc24msg.cfg \
                mbox/Readme.txt mbox/wc24recv.ctl mbox/wc24recv.mbx mbox/wc24send.ctl mbox/wc24send.mbx; do \
           curl -fsSL "$base/Wii/shared2/wc24/$f" -o "/opt/mkwps3/wii_bootstrap/shared2/wc24/$f" || exit 1; \
       done \
    && echo "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179  /opt/mkwps3/dsp_coef.bin" | sha256sum -c -

# The PS3's homebrew package key, which tools/mkpkg.py encrypts packages with. It's been
# public for years and ships in PSL1GHT's own packaging tools; it's taken from there
# rather than kept in this repository.
RUN curl -fsSL "https://raw.githubusercontent.com/ps3dev/PSL1GHT/$PSL1GHT_REV/tools/geohot/include/oddkeys.h" \
        | sed -n 's/^u8 retail_pkg_aes_key\[\] = {\(.*\)};/\1/p' | tr -d ' ,' | sed 's/0x//g' > /opt/mkwps3/pkg-key \
    && [ "$(tr -d '\n' < /opt/mkwps3/pkg-key | wc -c)" = 32 ]

# A copy of the source, for running the image on its own. build.sh mounts your
# checkout over it instead.
COPY . /repo

ENTRYPOINT ["/repo/tools/make-pkg.sh"]
