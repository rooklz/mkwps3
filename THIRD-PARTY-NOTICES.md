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

## Not included

No game data, Nintendo or Sony SDK files, firmware, or console keys are part
of this repository. The two runtime data files the port loads at run time
(`dsp_coef.bin` and the WiiConnect24 first-run tree) are Dolphin's own freely
licensed replacements and are fetched by the user from Dolphin's `Data/Sys`
directory (see `README.md`).

## Build-time dependencies, not redistributed

- **PSL1GHT** and the ps3dev toolchain (open-source PS3 homebrew SDK).
- **.NET 8 SDK** for the translator.
