# Working on MKW PS3

This is for anyone changing the code, people and coding agents alike (Claude Code picks it
up through `CLAUDE.md`). Read it before your first change. [docs/BUILDING.md](docs/BUILDING.md)
covers setting up, and [docs/HOW-IT-WORKS.md](docs/HOW-IT-WORKS.md) the big picture.

## Ground rules

- **Nothing from the game goes in the repo.** No files from the disc, nothing the translator
  generates from them (`generated/`, `build/` and the rest of the workspace), no Nintendo or
  Sony SDK code, no keys. The package key comes from PSL1GHT's sources at build time.
- **The real game is the reference.** Behaviour should match Mario Kart Wii on a Wii (Dolphin
  is a good stand-in). If the Wii does it too, it isn't a bug.
- **Say what you actually tested.** If a change hasn't run on a PS3, the PR says so.
- **Pick up an issue first** for anything bigger than a small fix, and comment on it so work
  isn't duplicated. The [full-speed tracking issue](https://github.com/rooklz/mkwps3/issues/20)
  lists the performance work.

## Where things are

| Path | What's there |
| --- | --- |
| `upstream/wiicompiled/translator/` | The PowerPC to C++ recompiler (C#) |
| `upstream/wiicompiled/projects/mkwii/` | The project file and symbol map (`MAP.txt`: guest addresses to names) |
| `runtime/` | The Wii side, C++17: OS, the GX FIFO, audio, disc, NAND, IOS, pads (`src/hle/`) |
| `renderer/src/video/rsx/` | GX to RSX, C11: draws (`gx_render.c`), TEV to fragment programs (`tev_program.c`), XF to vertex programs (`xf_program.c`), EFB copies, the display (`rsx_video.c`) |
| `renderer/src/core/gx/` | GX register state (BP, CP, XF) |
| `renderer/spu/` | The SPU vertex decoder |
| `platform/ps3/` | Start-up, lv2 glue, the control port (`rescue_ps3.cpp`), the SPU audio mixer (`spu/`) |
| `tools/` | Build, deploy, console and profiling scripts |
| `tests/` | Host tests: `audio/` (AX decoding and the SPU mixer), `renderer/` (shader dump tools) |

## Tests

```sh
dotnet test upstream/wiicompiled/translator/Translator.sln -c Release
make -C tests/audio                       # AX decoder, resampler, SPU mixer vs the C++ mixer
shellcheck -S error build.sh tools/*.sh
python3 -m py_compile tools/*.py
```

CI runs all four on every push and pull request. Audio changes must stay bit-exact: extend
`tests/audio` rather than loosen it.

## On a console

Everything is driven remotely (setup in [docs/BUILDING.md](docs/BUILDING.md#the-developer-way)):
`tools/build-ps3.sh` builds, `tools/deploy-ps3.sh` pushes the build and relaunches the app,
`tools/ps3-log.sh` prints the log.

| Script | What it does |
| --- | --- |
| `to-race.sh` | Drives the menus into a Grand Prix race (`AUTO=1`: the game's CPU driver races your kart) |
| `bench-gp.sh [windows]` | The benchmark: a race with the CPU driver at the wheel, averaged over 5-second windows |
| `ps3-shot.sh [file]` | Screenshot of the next frame |
| `ps3-pad.sh` | Presses buttons on a virtual controller |
| `ps3-pcs.sh` | Profiles the translated game code by function |
| `ps3-recover.sh` | Hard-reboots the console through webMAN when the RSX has locked up |

The control port (TCP 4001, `printf '<command>' | nc <console> 4001`) also takes: `gfx`
(renderer state), `prof` (renderer phase profile; the first call arms it), `ftrace [from]`
(every draw and EFB copy of the next frame, read with `tools/ftrace.py`), `cshot <n>` (an EFB
copy's pixels), `dinfo <n>|t<addr>|s<stages>` (one draw's full state), `dwin`/`dskip <min> <max>`
(draw only, or skip, a range of draws), `tevshow`/`xfshow` (show shader intermediates),
`peek <addr> <len>` and `poke <addr> <value>` (guest memory), `where`, `fault`.

Running anything on someone's console changes it: deploying relaunches the app and
`ps3-recover.sh` reboots the machine. Check before doing either on hardware that isn't yours.

## Things the PS3 will catch you out with

- **A faulted RSX stays faulted.** The app relaunches fine, then hangs in RSX init until the
  console is rebooted (`tools/ps3-recover.sh`). If the log stops at `[gfx] before RSX init`,
  that's what happened.
- **Don't use RSX reports.** PSL1GHT's `rsxSetTimeStamp` writes its data word to the wrong
  slot, and the report method faulted the RSX here even when written correctly. The renderer
  measures with backend labels instead (`rsx_fence_emit`).
- **The PPE is in-order, with two hardware threads on one core.** A load that hits a recent
  store costs ~50 cycles, there are no direct moves between integer and float registers, and
  anything the renderer thread does slows the game thread down.
- **SPU DMA is strict.** Transfers are 1, 2, 4, 8 or a multiple of 16 bytes, naturally aligned
  with matching low address bits on both sides, or the SPU thread dies.
- **RSX fragment programs need an even register count.**
- `std::snprintf` isn't declared in some translation units with this newlib; use `snprintf`.
- New native functions (`REGISTER_NATIVE_FUNCTION_AS`) need the translator's
  `emit-build-shards` step re-run so direct calls bind to them.

## Speed changes

Run `tools/bench-gp.sh 8` before and after on the same console and put both lines in the PR.
Runs vary by a few percent, so run again if the difference is small. Measurement scaffolding
stays out of normal builds: behind a flag, a build option, or switched on by the control port.

## Visual and audio changes

Before and after screenshots for anything you can see, and how you compared it with the real
game. For audio, the tests above.

## Code

- Match the file you're in: naming, layout, how much it comments. C in `renderer/`, C++17 in
  `runtime/`.
- Comments say why, especially for hardware and game quirks, with the evidence (a measurement,
  an address, a register). Read the comments around code before changing it; a lot of them
  record why the obvious version didn't work.
- Guest addresses, struct offsets and register layouts come from the code, the symbol map or
  a disassembly, never from memory. Say in the PR where they came from.
- No new dependencies without an issue first.
- British English in comments and docs.

## Commits and pull requests

- Branch from `main`, one change per pull request, and link the issue (`Fixes #12`).
- Commit subjects start with the area: `Renderer:`, `Audio:`, `Runtime:`, `Translator:`,
  `Build:`, `Tools:`, `Docs:`. The body says why, with numbers where there are any.
- No co-author or tool attribution trailers in commits. If a tool helped, the PR description
  can say so.
- Fill in the pull request template: what and why, how it was tested (console model,
  firmware, CFW or HEN), and the numbers.
- User-visible changes get a line in `CHANGELOG.md` under Unreleased.
- No drive-by reformatting or refactoring. It hides the real change and won't be merged.
