# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build

This is a macOS fork of libayemu. All builds use GNU Autotools via the convenience wrapper:

```bash
make -f Makefile.macos          # full bootstrap → configure → build
make -f Makefile.macos build    # recompile only (after initial configure)
make -f Makefile.macos clean
make -f Makefile.macos distclean  # removes all autotools-generated files
```

After the first `make -f Makefile.macos`, incremental rebuilds use plain `make` from the repo root.

The `--disable-tests` flag is set by default in `Makefile.macos` because `test/test.c` targets OSS (`/dev/dsp`) and does not build on macOS.

To play a sample file after building:
```bash
./apps/playvtx/playvtx music_sample/ritm-4.vtx
./apps/playvtx/playvtx --stdout music_sample/secret.vtx | sox -r 44100 -e signed -b 16 -c 2 - output.mp3
```

## Architecture

### Library (`src/` + `include/`)

`libayemu` emulates the AY-3-8910 / YM2149 sound chips used in ZX Spectrum 128K and Atari ST. The public API is in three headers under `include/`; `ayemu.h` is the umbrella that includes the other two.

- **`ay8912.c`** — chip emulation core: volume tables, envelope state machine, stereo mixing, PCM generation via `ayemu_gen_sound()`
- **`vtxfile.c`** — VTX file parser: reads the header and unpacks LH5-compressed register frames
- **`lh5dec.c`** — LH5 decompressor used by the VTX parser

Typical usage pattern (also how `playvtx` uses it):
1. `ayemu_vtx_load_from_file()` → loads VTX, returns `ayemu_vtx_t *` with `frames` count and metadata
2. `ayemu_init()` + `ayemu_set_chip_type/freq/stereo/sound_format()` — configure the emulator once
3. Per-frame loop: `ayemu_vtx_getframe()` → `ayemu_set_regs()` → `ayemu_gen_sound()` into a PCM buffer

### Player (`apps/playvtx/playvtx.c`)

Single C file with two platform backends selected at compile time:

- **CoreAudio path** (`#ifdef __APPLE__`, `play_coreaudio()`): AudioQueue push model with 3 preallocated buffers. Main thread waits on `dispatch_semaphore`; callbacks render and re-enqueue frames. Buffer size = one VTX frame worth of samples (`freq / playerFreq` frames × bytes per frame).
- **OSS path** (`play()`): synchronous per-frame write loop to `/dev/dsp`, also used for `--stdout` output on all platforms.

Esc-to-stop is implemented with POSIX `termios` raw mode + non-blocking stdin. `term_raw_enable()` is called from `main()` when `!sflag`; `poll_esc()` is called once per frame in both paths. The `stop_requested` volatile flag is shared between the main thread and the CoreAudio callback. Terminal state is restored via `atexit(term_restore)`.

### Autotools / Platform Detection

`configure.ac` uses `AC_CANONICAL_HOST` to detect Darwin and set a `HOST_DARWIN` AM conditional. `apps/playvtx/Makefile.am` uses this to add `-framework CoreAudio -framework AudioToolbox` on macOS. `Makefile.macos` uses `glibtoolize` (macOS name for `libtoolize`).

## Key Files

| File | Role |
|------|------|
| `include/ayemu_8912.h` | Chip emulation API — types, functions |
| `include/ayemu_vtxfile.h` | VTX file API — `ayemu_vtx_t`, loader functions |
| `src/ay8912.c` | Emulation engine |
| `apps/playvtx/playvtx.c` | Player — only file changed for macOS features |
| `configure.ac` | Autotools root config — Darwin detection lives here |
| `apps/playvtx/Makefile.am` | CoreAudio linker flags |
| `Makefile.macos` | Convenience build wrapper |
| `music_sample/` | 13 VTX files for manual testing |
