# Building

There are two ways to build MKW PS3:

- **The easy way:** one command with Docker, which gives you an installable `.pkg`.
  This is what the [README](../README.md#getting-it-running) walks through.
- **The developer way:** the same steps by hand, with builds pushed straight to a
  console on your network. Worth it if you're going to be changing things.

Either way you need your own clean PAL (`RMCP01`) disc image of Mario Kart Wii.

## The easy way, in more detail

`build.sh` (or `build.ps1` on Windows) runs everything inside a Docker image that has
the PS3 toolchain, .NET and the disc tools already in it:

1. **Checks the disc.** It has to be `RMCP01`, and `main.dol` and `StaticR.rel` have to
   match the clean release byte for byte.
2. **Extracts it** with [nodtool](https://github.com/encounter/nod-rs).
3. **Builds the translator** (C#) from this repo.
4. **Translates the game:** PowerPC code in, C++ out. About 30,000 functions.
5. **Compiles** all of it for the PS3. This is the slow part.
6. **Packs** the result and the game's files into `out/MKWP00001.pkg`.

All the in-between files live in a Docker volume called `mkwps3-work`, so running the
build again only redoes what's changed. To start completely fresh:

```sh
docker volume rm mkwps3-work
```

### Options

These are environment variables, set before running the script:

| Variable | What it does |
| --- | --- |
| `JOBS` | How many files to compile at once. Worked out from Docker's memory by default; set it lower if the build runs out of memory. |
| `MKWPS3_IMAGE` | Which builder image to use. Defaults to `ghcr.io/rooklz/mkwps3-builder:latest`. |
| `MKWPS3_VOLUME` | Which Docker volume holds the workspace. Defaults to `mkwps3-work`. |

For example, `JOBS=2 ./build.sh ~/Games/MarioKartWii.wbfs`.

### If something goes wrong

- **"Docker isn't installed" / "isn't running"**: install
  [Docker Desktop](https://www.docker.com/products/docker-desktop/), open it, wait until
  it says it's running, and try again.
- **"only the PAL version (RMCP01) is supported"** or **"doesn't match the clean PAL
  release"**: the disc is a different region, or it's been modified (a Riivolution
  patch, a custom track pack and so on). Dump a clean copy.
- **The compile stops with "Killed" or "out of memory"**: give Docker more memory
  (Docker Desktop → Settings → Resources), or run with `JOBS=2`.
- **Downloading the builder image fails**: the script falls back to building the image
  itself. That compiles the whole PS3 toolchain and takes about an hour, but it only
  happens once.
- **Anything else**: the full logs are in the workspace volume.
  `docker run --rm -v mkwps3-work:/ws debian:12-slim tail -50 /ws/compile.log`
  shows the end of the compile log; swap in `translate.log` for the translation.
  If you're stuck, open an issue with that output.

### Building the builder image yourself

The image comes in two layers: a PS3 toolchain image (GCC 7.2, binutils and PSL1GHT,
every source pinned) and the builder on top of it. CI publishes both, but you can build
them locally:

```sh
docker build -f docker/toolchain.Dockerfile -t mkwps3-toolchain:local .
docker build --build-arg TOOLCHAIN=mkwps3-toolchain:local -t ghcr.io/rooklz/mkwps3-builder:latest .
```

## The developer way

This is how the port is actually worked on: build on your machine, push the new build to
the console over the network, relaunch it remotely, read its log, profile it, take
screenshots, all without touching the PS3.

### What you need

- A PS3 running CFW with [webMAN MOD](https://github.com/aldostools/webMAN-MOD) (for FTP
  and remote launching), on the same network as your computer.
- Docker, with the toolchain image above (or your own install of ps3toolchain and
  PSL1GHT at `/usr/local/ps3dev`).
- The [.NET 8 SDK](https://dotnet.microsoft.com/download/dotnet/8.0) for the translator.
- Python 3.
- Your disc extracted with `nodtool extract <disc> <folder>`.
- Dolphin's `Data/Sys/GC/dsp_coef.bin` and `Data/Sys/Wii/shared2/` (the
  [Dockerfile](../Dockerfile) shows exactly which revision).

### Setting up

1. Copy `tools/local.env.example` to `tools/local.env` and fill it in: the workspace
   folder (`GEN`), your PS3's IP address, and the folder the app is installed in
   (`PS3_APP`, normally `MKWP00001`). This file is ignored by git.
2. Prepare the workspace:
   - copy `upstream/wiicompiled/projects/mkwii` to `$GEN/projects/mkwii`;
   - put `main.dol` (from `sys/`) and `StaticR.rel` (from `files/rel/`) in `$GEN/Assets/`;
   - make `$GEN/runtime` a symlink to this repo's `runtime/`.
3. Build the translator:
   ```sh
   dotnet build upstream/wiicompiled/translator/src/Translator.Cli/Translator.Cli.csproj -c Release
   ```
4. Translate: `tools/translate.sh`
5. Compile: `tools/build-ps3.sh` (it runs the toolchain in Docker). The output lands in
   `$GEN/build/ps3`.
6. Install the app once, either from a package made by the easy route, or by hand:
   `EBOOT.BIN`, `RELOAD.SELF`, `vtx_spu.elf`, `ax_spu.elf` and `mkwii_sections.bin` in
   `/dev_hdd0/game/MKWP00001/USRDIR/`, plus `dsp_coef.bin`, `wii_bootstrap/shared2/`, and
   the disc's `files/` and `sys/` under `USRDIR/DATA/` (`tools/push-data.py` uploads
   those over FTP).

### The loop

- `tools/cycle.sh` builds, pushes the new build to the console, relaunches it and prints
  the log.
- `tools/deploy-ps3.sh` just pushes and relaunches.

Translation only needs redoing when the translator, the project file or the runtime's
native function list changes. Runtime-only changes are a quick incremental compile.

### Talking to the running app

The app listens on TCP port 4001. The scripts in `tools/` wrap it:

| Script | What it does |
| --- | --- |
| `ps3-log.sh` | Prints the app's log (`/dev_hdd0/tmp/mkwii-ps3.log`) |
| `ps3-shot.sh` | Saves a screenshot of the next frame |
| `ps3-pad.sh` | Presses buttons on a virtual controller, for driving menus remotely |
| `ps3-pcs.sh` | Samples where the game thread spends its time and prints a profile |
| `to-race.sh` | Drives the menus into a race and leaves it there (`AUTO=1`: the game's CPU driver races your kart) |
| `bench-gp.sh` | The standard benchmark: a 50cc Grand Prix with the game's CPU driver racing your kart, reporting frame times |
| `ps3-recover.sh` | Hard-reboots the console through webMAN and relaunches the app, for when the GPU has locked up |

The log's `[stats]` line, every five seconds, is the quickest way to see how fast things
are: frames per second, how busy the game thread is, the audio mix's cost, memory, and
what the GPU did.

### Tests

```sh
# The translator's test suite (no game files needed)
dotnet test upstream/wiicompiled/translator/Translator.sln -c Release

# The audio decoder and resampler, checked against reference versions, and the SPU
# mixer checked against the C++ one
make -C tests/audio
```

Both run in CI on every push, along with shellcheck and a syntax check of the Python tools.
[AGENTS.md](../AGENTS.md) has the full list of console commands and the PS3's pitfalls.
