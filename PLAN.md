# Mario Kart Wii on PS3 — native port plan

The previous attempt ran WiiCompiled's translated main.dol inside a Dolphin-derived
emulator shell: StaticR.rel (the race logic) stayed in a JIT, Wii hardware (Bluetooth,
IOS, DSP, the GX FIFO) was emulated underneath, and the game thread sat near 120 ms a
frame. This port starts from the official WiiCompiled architecture instead: the whole
game (DOL and REL) is translated to C++, and the Wii SDK is replaced at the API level
(OS, GX, AX, PAD, DVD, IOS). Nothing is emulated.

## Pipeline

1. Disc -> `main.dol`, `StaticR.rel` (PAL, RMCP01; hashes pinned in
   `upstream/wiicompiled/projects/mkwii/recomp.yml`). Extracted with nodtool, kept
   outside the repository.
2. WiiCompiled translator (C#, .NET 8) -> 29,637 C++ functions, emitted as 72 build
   shards plus the indirect-dispatch and native-registration tables.
3. PS3 build: shards + `runtime/` (the WiiCompiled runtime, forked for the PS3) +
   aurora's GX API layer + the PS3 platform layer -> SELF.

## Architecture on the PS3

- **Guest memory:** a 1 GiB arena addressed through a pinned register; guest
  addresses fold with `& 0x3FFFFFFF`, so the cached and uncached mirrors land on the
  same bytes. MEM1 and MEM2 are backed. Big-endian host: no byte swapping.
- **Guest threads:** fibers on the game PPU thread (WiiCompiled's OS HLE schedules
  them; no interrupt emulation).
- **Graphics:** WiiCompiled's GX HLE drives aurora's GX API, which emits standard GX
  FIFO bytes. That stream goes to the PS3 renderer (FIFO parser, BP/XF/CP state, TEV
  and XF program generators for NV40, EFB copies, draw batching, SPU vertex decode)
  on the second PPU thread, in place of aurora's WebGPU backend.
- **Audio:** the AX HLE mixer, moved onto an SPU; output through libaudio.
- **Input:** PS3 pads presented to the game as GameCube controllers (as upstream).
- **Disc:** files read from the extracted image on the HDD.

## Phases

1. Headless boot: game logic native on the PS3, rendering discarded. Measure the
   game thread per frame; this decides how much translator work 60 fps needs.
2. Rendering through the ported RSX backend.
3. Audio, input, saves.
4. Performance: Cell-specific code generation (register residency across calls,
   load-hit-store avoidance, paired singles on VMX), SPU offload of FIFO parsing,
   vertex decode and audio.

## Licence

WiiCompiled is GPL-3.0; this port is a derivative and is GPL-3.0 as well. No game
code, data, or translator output is ever committed.
