# Mario Kart Wii for PS3

A native PlayStation 3 port of Mario Kart Wii (PAL, RMCP01), built on the
[WiiCompiled](https://github.com/patchzyy/Wiicompiled) static recompiler. The
game's PowerPC code is translated ahead of time into C++ and compiled for the
Cell PPE; the Wii hardware it talks to is reimplemented natively for the PS3.

**Status:** boots, plays the title movie, goes through the menus and runs
races, currently at roughly 20-30 fps in a 12-kart race. Work is ongoing to
reach full speed.

You need your own copy of the game. Nothing from the disc is in this
repository, and nothing it builds can run without it.

## How it works

- **CPU:** the translated game runs on one PPE hardware thread, against a 1 GiB
  guest arena with a pinned base register, so guest loads and stores are
  single host instructions. Both machines are big-endian PowerPC, so there is
  no byte swapping.
- **GX:** the game's own GX library runs translated. Its write-gather pipe
  stores go into a ring that a GPU thread on the second PPE hardware thread
  parses (`runtime/src/hle/gxhw`).
- **Rendering:** GX state is translated to the RSX (`renderer/`). TEV and XF
  configurations become fragment and vertex programs. Vertex decoding, skinning
  and index generation run on five SPUs (`renderer/spu/vtx_spu.c`). Runs of
  primitives from one display list become a single SPU job and a single
  indexed RSX draw.
- **System:** OS, DVD, NAND, audio (AX/DSP), pads and IOS are high-level
  implementations in `runtime/src/hle`. THP video decoding is native, with an
  AltiVec IDCT.

## Layout

| Path | Contents |
| --- | --- |
| `runtime/` | the WiiCompiled runtime, ported to the PS3 |
| `renderer/` | GX to RSX renderer and the SPU vertex decoder |
| `platform/ps3/` | lv2 entry point, threads, files, pads, audio, control port |
| `upstream/wiicompiled/` | the translator and the Mario Kart Wii project definition |
| `tools/` | build, deploy and on-console debugging scripts |
| `Makefile.ps3` | the PS3 build |

## Requirements

- A PS3 that runs homebrew, with webMAN MOD for FTP.
- A Docker image `ps3dev:latest` providing the ps3dev toolchain and PSL1GHT at
  `/usr/local/ps3dev`.
- The .NET 8 SDK, for the translator.
- Mario Kart Wii PAL (RMCP01): `main.dol` and `StaticR.rel`, plus the disc's
  `files/` and `sys/` trees.
- Two data files from [Dolphin](https://github.com/dolphin-emu/dolphin)'s
  `Data/Sys` directory, placed in `local/` (not committed):
  - `local/dsp_coef.bin`, from `Data/Sys/GC/dsp_coef.bin`
  - `local/wii/shared2/`, from `Data/Sys/Wii/shared2`

## Building

1. Copy `tools/local.env.example` to `tools/local.env` and fill it in.
2. Prepare the workspace (`GEN`):
   - copy `upstream/wiicompiled/projects/mkwii` to `projects/mkwii`;
   - put `main.dol` and `StaticR.rel` in `Assets/`;
   - make `runtime` a symlink to this repository's `runtime/`.
3. Build the translator:
   `dotnet build upstream/wiicompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release`
4. `tools/translate.sh`: translates the game into C++ build shards.
5. `tools/build-ps3.sh`: compiles everything, producing `EBOOT.BIN`,
   `RELOAD.SELF`, `vtx_spu.elf` and `mkwii_sections.bin` in `$GEN/build/ps3`.

The first full build takes about 35 minutes; later builds only recompile what
changed.

## Running

- Install the app as `/dev_hdd0/game/DOLPHIN01` with a `PARAM.SFO`.
- Upload the disc files with `tools/push-data.py <extracted DATA dir>`.
- `tools/deploy-ps3.sh` pushes the build and relaunches the app over its
  control port.

The app listens on TCP port 4001 for `relaunch`, logs, PC and stack sampling,
renderer statistics, screenshots and pad input. `tools/` wraps these
(`ps3-log.sh`, `ps3-pcs.sh`, `ps3-shot.sh`, `ps3-pad.sh`, `to-demo.sh`).
`tools/ps3-recover.sh` hard-reboots the console through webMAN and relaunches
the app when the GPU has stopped responding.

## License

GPL-3.0, as a derivative of WiiCompiled. See `LICENSE` and
`THIRD-PARTY-NOTICES.md`.
