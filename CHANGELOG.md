# Changelog

What's changed, newest first. Dates are when it landed on `main`.

## Unreleased

The first public version.

- **One-command builds.** `build.sh` / `build.ps1` turn your own PAL disc image into an
  installable `MKWP00001.pkg`, with Docker as the only thing to install.
- **Its own icon and background on the XMB**, and the app now runs from whatever folder it's
  installed in.
- **Audio mixing roughly 40% cheaper.** Idle voices are skipped, ADPCM is decoded in runs, and
  the resampler no longer stalls the PPE. Checked bit-for-bit against the old code.
- **Translator:** registers stay in registers across calls that follow the Wii's calling
  convention, native single-precision maths, and fewer pointless save-and-reload pairs. Together
  with the system functions now done natively, a race frame went from about 35ms to 23ms on the
  game thread.
- **Races run at about 38fps instead of 30.** The renderer has a fourth display buffer, so a
  frame no longer waits on the flip before last.
- **The audio mix runs on an SPU.** Checked against the PPU mixer bit for bit.
- **Karts and drivers look right.** They were dark silhouettes in races; the game asks for a
  fixed alpha on them that wasn't being honoured. Lights were also the wrong colour.
- **Bloom works.** It used to wash the screen out everywhere but the top-left corner.
- **Rendering:** vertex decoding, skinning and index building spread across the SPUs (now on by
  default), with runs of draws batched into single RSX draws.
- Exceptions thrown inside the game thread are caught properly again (a linker quirk had broken
  them).
