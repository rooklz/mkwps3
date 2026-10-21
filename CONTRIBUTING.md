# Contributing to MKW PS3

Cheers for wanting to help. Here's how it works.

## The short version

- Never put Nintendo or Sony code, game files, keys or anything from a disc in an issue, a
  pull request or anywhere else in this project. No exceptions.
- You need to understand, and be able to explain, every line you submit.
- Write your own pull request descriptions and issue replies.
- For anything that touches how the game behaves, matching the real game on a Wii is the bar.

## Reporting bugs

Use the bug report form when you
[open an issue](https://github.com/rooklz/mkwps3/issues/new/choose). The things that
make a report actually useful:

- **The log.** `/dev_hdd0/tmp/mkwii-ps3.log` on the console. webMAN's file manager or FTP
  will get it off.
- **What you were doing**, and whether it happens every time.
- **A photo or screenshot**, for anything visual.
- **Whether the real game does it too.** If it happens on a Wii or in Dolphin as well, it's
  the game, not the port, and there's nothing to fix.

Questions, ideas and "has anyone tried this on HEN?" are better off in
[Discussions](https://github.com/rooklz/mkwps3/discussions).

## Pull requests

- **One thing per pull request.** Small ones get looked at quickly; huge ones sit there.
- **Say what you changed and why**, and link the issue if there is one.
- **Test it on a real PS3 if you possibly can**, and say how you tested it. The PS3 is fussy in
  ways nothing else is, and "it compiles" doesn't mean much here.
- **Keep to the style of the code around it.** Same naming, same comment density, same idioms.
- **Run the tests** (see [docs/BUILDING.md](docs/BUILDING.md#tests)). CI runs them too.
- **Speed changes need numbers.** `tools/bench-gp.sh` before and after, on the same build
  otherwise.
- **Commits** should have a short first line saying what changed, and a body saying why if
  it's not obvious.

### Where help would make the biggest difference

- **The renderer.** Karts and characters render as dark silhouettes, bloom shows up as boxes
  and Mii faces are wrong. Anyone who knows the RSX or the Wii's TEV well would be a huge help.
- **Speed.** Anything that gets the game thread from ~23ms a frame towards 16.7ms. The audio mix
  moving to an SPU is the next big obvious one.
- **Testing** on hardware other than mine: Slims, Super Slims, HEN, different firmwares.

## If you use AI tools

Fine, but the same rules apply as for everything else. You have to be able to explain your
change when asked, "the AI wrote it" isn't an explanation, and the PR description has to be
yours. Low-effort generated stuff gets closed, the same as low-effort hand-written stuff.

## Licence

By contributing you agree your work is licensed under the [GPL-3.0](LICENSE), same as the rest
of the project.

Everyone taking part is expected to follow the [code of conduct](CODE_OF_CONDUCT.md).
