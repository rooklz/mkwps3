# Lessons learned

What this port taught us about running a statically recompiled Wii game on the PS3, written
down for anyone who forks it or tries something similar. The numbers are from a real PS3
running a Grand Prix race on Luigi Circuit, timed over race frames 600 to 3000 with the
same seeds every run.

## The short version

- The game runs at about 41fps, so the race plays at about two-thirds speed. That's
  because the game counts time in frames.
- On the PS3's main core (the PPE), the game's own translated code needs about 21ms a frame;
  60fps allows 16.7ms. With the renderer switched off entirely it still needs 18.7ms.
- The profile is flat. About 200 functions each take 0.05 to 0.4ms, and none is worth
  hand-tuning on its own.
- Every large idea we had was measured on the console, and none closed the gap.

## Why the PPE struggles

- **It's in-order.** It can't run ahead past a cache miss. A load that hits a recent store
  costs about 50 cycles, and there are no direct moves between integer and float registers.
- **The two hardware threads share one core.** Each millisecond of renderer work on the
  second thread cost the game thread about 0.15ms, through the shared pipeline and the
  shared 512 KB L2. With the renderer gone, game time fell from 21.0 to 18.7ms.
- **Code size matters as much as instruction count.** 80% of game-thread time ran in 480 KB
  of host code, and 90% in 830 KB. That's more than the whole L2, so instructions keep
  coming from main memory. About 20 MB of translated code that never runs still cost 0.8%,
  just by being in the binary.
- **The translation itself is tight.** It produces about 1.4 host instructions per guest
  instruction. The cost is in how fast the PPE gets through them: about 8 cycles per guest
  instruction overall, across roughly 8 million guest instructions a frame.

## What we tried

| Idea | Result |
| --- | --- |
| A native PowerPC-to-Cell backend instead of C++ | 2.5× slower (52 against 21ms of game time); crossing between native and C++ code cost more than it saved |
| Passing arguments in registers between translated functions | No gain (+0.09ms) |
| Keeping callee-saved registers local to each function | Kept: −0.15ms |
| Hand-written native versions of hot library functions (memcpy, matrix maths, voice mixing) | Correct, but about 0.1ms each against a flat profile |
| Profile-guided optimisation (PGO) of the hot translated code | −1.4ms on the game thread and 17% less hot code, but +0.5ms on the renderer, so only +0.3fps. Never merged |
| Moving audio work to the SPUs | Slower; the audio blocks run back to back, so each SPU job's wait just moved |
| Parsing the renderer's command stream on the SPUs | Kept: renderer 17.2 → 15.5ms per frame |
| A 30fps mode: run the game every frame but draw every other one | 0.93× speed, close to full, but some effects ran at half rate. The game advances some state inside its draw code, and proving which draw work is safe to skip would have needed much more tooling |

## How we measured, and what fooled us

- **Benchmark on hardware, in pairs.** We alternated builds ABBA over three pairs, so drift
  from heat or background work landed on both. Run-to-run noise was about 0.5% on game
  time, and anything smaller was treated as noise.
- **Fingerprint the output.** A hash of each frame's graphics commands at fixed race frames
  caught every change that altered the game. A speed-up that changes the fingerprint isn't
  a speed-up.
- **Background tools on the console cost real speed.** A homebrew web server running during
  benchmarks cost 4.8fps until we noticed. Several early results were wrong because of it.
- **The scripts break too.** A runtime change added fields to a status line, and an older
  benchmark script waited for the old format forever. Check that a job actually started
  and is making progress.
- **Only a race that starts from a fresh boot repeats exactly.** Races 2 to 4 of a cup
  differed between nights, because the time spent on results screens moves the random
  number generator.
- **The game's own staff ghosts make good physics tests.** The disc carries ghosts recorded
  on real hardware, along with their finish times. Replaying them should land on the same
  times exactly. We planned this check but didn't get to run it.

## Working with AI coding agents

The project was written almost entirely by Claude Code agents, several at once. Some of
that went well and some didn't:

- One shared console was the real bottleneck. A lock around every console job was
  essential, and so was chaining each job onto the build it needed. Twice the console sat
  idle for hours because a job waited on a log line that never came.
- Wait on a process ID or a file, never on a process-name pattern; such a wait can match its
  own command line. Kill only processes you started.
- Agents report their own work optimistically. A clean-up that "deleted" folders hadn't
  run, and a result reported as verified hadn't been checked. Anything that matters needs
  checking independently.
- Agree on stop rules before starting a big idea: "if it isn't under X ms by step Y, stop".
  The native backend ran for days before its numbers were in.

## If you carry it on

The realistic directions we saw, none of them a sure thing:

- Make the translated code smaller, not just faster: PGO, hot and cold splitting, and
  leaving never-run code out of the binary.
- Finish the 30fps mode. It needs a way to prove which draw work can be skipped without
  changing anything the game reads later, for example by recording every memory write the
  skipped draw would have made and checking nothing reads them before they're overwritten.
- Move more of the renderer's work off the PPE. Every millisecond moved buys back about
  0.15ms of game time.
