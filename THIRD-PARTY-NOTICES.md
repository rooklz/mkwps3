# Third-Party Notices

This repository is a derivative of WiiCompiled and is distributed under the
GNU General Public License v3 (see `LICENSE`).

## WiiCompiled - GPL-3.0

The static recompiler (`upstream/wiicompiled/translator`), the Mario Kart Wii
project definition (`upstream/wiicompiled/projects`) and the runtime this port
is built on (`runtime/`, modified) come from WiiCompiled. Its own notices are
kept in `upstream/wiicompiled/THIRD-PARTY-NOTICES.md` and `upstream/wiicompiled/LICENSE`.

## Vendored libraries

- **pugixml** 1.15 - MIT, Copyright (c) 2006-2025 Arseny Kapoulkine.
  `runtime/third_party/pugixml`.
- **Crypto++** 8.9.0 - Boost Software License 1.0, Copyright (c) 1995-2019 Wei
  Dai and contributors. `runtime/third_party/cryptopp`.
- **toml11** 4.4.0 - MIT, Copyright (c) 2017 Toru Niina.
  `runtime/third_party/toml11`.

## Dolphin Emulator - GPL-2.0-or-later

Parts of the runtime's Riivolution support are ports of Dolphin code (marked
in the files). Dolphin was also used as a behavioural reference for the GX,
DSP and IOS implementations. <https://github.com/dolphin-emu/dolphin>

## Fonts

- **Michroma** - SIL Open Font License 1.1, Copyright (c) 2011 Vernon Adams. Used for the
  type in `docs/press/`; the font file itself isn't included.

## Not included

No game data, Nintendo or Sony SDK files, firmware, or console keys are part
of this repository. The build pulls in, at build time and from their own
projects, at pinned revisions (see `Dockerfile`):

- Dolphin's freely licensed replacement DSP coefficient ROM (`dsp_coef.bin`) and
  its default WiiConnect24 tree, both loaded by the runtime.
- The PS3's homebrew package key, from PSL1GHT's packaging tools, used by
  `tools/mkpkg.py` to write installable packages.

## Build-time dependencies, not redistributed

- **PSL1GHT** and **ps3toolchain** - the open-source PS3 homebrew SDK and compilers.
- **nod** (nodtool) - MIT, for reading disc images.
- **.NET 8 SDK** for the translator.
