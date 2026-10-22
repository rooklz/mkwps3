# How it works

A Wii game is a big lump of PowerPC machine code that expects to be running on a Wii.
The PS3 happens to have a PowerPC core too, the PPE at the heart of the Cell, but that
doesn't mean you can just run the code. Everything around the CPU is different: the
graphics chip, the sound chip, the controllers, how memory is laid out, the operating
system underneath. So MKW PS3 does it in two halves.

## Half one: translating the game

[WiiCompiled](https://github.com/patchzyy/Wiicompiled)'s translator reads the game's
executable (`main.dol`) and its race code (`StaticR.rel`) and turns every function it can
find, about thirty thousand of them, into C++. That C++ gets compiled with an ordinary PS3
compiler (GCC 7.2 from the homebrew toolchain), so by the time you press play it's just
native code, with nothing interpreting or emulating the Wii's CPU.

The interesting bit is getting that translated code to run *fast* on the Cell's PPE,
which is fussy in its own particular ways:

- **It's in-order.** It can't shuffle instructions around while it waits for something,
  so a slow instruction holds everything up behind it.
- **Load-hit-store.** Writing a value to memory and reading it straight back stalls for
  around 50 cycles. Naively translated code does this constantly, because the Wii
  version keeps passing values through a big "CPU state" structure in memory.
- **Calls cost more than you'd think**, for the same reason.

So the translator here has been taught to keep registers in real registers across calls
when it can prove that's safe (it trusts the Wii's own calling conventions instead of
saving and restoring everything), to swap the most-called little system functions for
direct native versions, to skip pointless save-and-reload pairs, and to use the Cell's
own single-precision maths. Each of those knocked milliseconds off a frame.

Both machines are big-endian PowerPC, which is a lucky break: memory doesn't need byte
swapping. The game's memory lives in one big block on the PS3, with its base address
pinned in a spare CPU register, so a guest memory access is a single PS3 instruction.

## Half two: pretending to be a Wii

The translated game still calls into the Wii's system libraries and pokes at Wii
hardware. All of that is reimplemented ("high-level emulation", HLE) in `runtime/`:

- **Graphics.** The game builds GameCube/Wii graphics command lists (the "GX" API) like it
  always would. A second PPE thread reads that command stream and translates it for the
  PS3's RSX: the Wii's fixed-function colour combiner (TEV) and transform unit become RSX
  shader programs, generated on the fly and cached. Texture formats get converted, and
  the Wii's tricks of copying bits of the screen into textures get recreated.
- **The SPUs.** Decoding vertices, skinning character models and building index buffers
  is exactly the kind of number-crunching the Cell's SPUs were made for, so four of them
  do it in parallel (`renderer/spu/`). Runs of draws get batched into single SPU jobs and
  single RSX draws. Another SPU reads the game's graphics command stream before the PPE
  does (`platform/ps3/spu/gxf_spu.c`): it keeps its own copy of the GPU's registers and
  only passes on the writes that actually change one, which is about 40% of them.
- **Audio.** The Wii has a DSP that mixes up to 96 voices. Here that mixer is native C++:
  it decodes the game's ADPCM samples, resamples them, applies the volume envelopes and
  filters and mixes them down, then hands the result to the PS3's audio output.
- **Everything else.** Threads, timers, the disc drive (reads come off the PS3's hard
  drive instead), saves (NAND, also on the hard drive), controllers (a DualShock 3 shows
  up as a GameCube pad), and the intro movie (THP video, decoded natively with AltiVec).

## Where the time goes

At the moment a full race costs the game thread about 21ms a frame on the PS3, plus about
2ms of audio, against the 16.7ms it needs for 60fps. The thread that turns the game's
graphics commands into RSX ones needs about 17ms, some of it waiting. The two share the
PPE, so each slows the other down. That works out at around 40fps. (The benchmark lets the game's own CPU driver race the player's kart,
so it sees the same track and traffic a real race does.)

The game itself is most of that: the PPE has to get through roughly eight million of the
Wii's instructions every frame, at around nine PS3 cycles each. The rest is the parts the
Wii had dedicated hardware for. The audio mix, for example, went from about 4.7ms to
2.8ms a frame on the PPE, and now runs on an SPU instead.

The plan from here is more of the same: better code from the translator, more of the
hottest system functions done natively, and moving work that doesn't need the PPE onto
the SPUs, the way the vertex work and the audio mix already have.

## Where to look in the code

| Path | What's there |
| --- | --- |
| `upstream/wiicompiled/translator/` | The translator (C#) |
| `upstream/wiicompiled/projects/mkwii/` | The Mario Kart Wii project file and symbol map |
| `runtime/` | The Wii side: OS, GX, audio, disc, NAND, IOS and pads (C++) |
| `renderer/` | GX to RSX translation, plus the SPU vertex program (C) |
| `platform/ps3/` | PS3 entry point, threads, files, audio output, pads, the remote control port |
| `tools/` | Build, deploy, packaging and on-console debugging scripts |
| `docker/`, `Dockerfile` | The build environment |
