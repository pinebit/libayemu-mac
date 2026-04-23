# playvtx macOS CoreAudio port — design

**Date:** 2026-04-23
**Fork:** `pinebit/libayemu-mac` (forked from `asashnov/libayemu`)
**Target audience:** macOS users who want to play `.vtx` files natively.

## Goal

Make `playvtx` play `.vtx` files directly through the macOS default audio
output, with no OSS wrappers, ALSA shims, or stdout piping required. Keep the
existing Linux/OSS build path unchanged so the fork stays a candidate for
upstream contribution.

Simplest user experience on macOS:

```sh
brew install autoconf automake libtool
make -f Makefile.macos install
playvtx music_sample/ritm-4.vtx
```

## Non-goals

- Porting `test/test.c` (also OSS-dependent; disabled on macOS via
  `--disable-tests`).
- Porting the `xmms-vtx` or `gstreamer-vtx` plugins.
- GUI, device selection, volume control, or `SIGINT` handling during playback.
- Prebuilt binaries, bottles, or a Homebrew tap.
- CI (GitHub Actions).

## Architecture

Single-file change in `apps/playvtx/playvtx.c`, guarded by `#ifdef __APPLE__`.
The existing OSS code path stays untouched. The library itself
(`src/`, `include/`) is already platform-agnostic and does not change.

```
libayemu-mac/
├── Makefile.macos           [NEW] convenience wrapper (bootstrap → configure → make)
├── README.md                [EDITED] trimmed to macOS focus, upstream pointer
├── configure.ac             [EDITED] +10 lines: AC_CANONICAL_HOST + HOST_DARWIN
├── apps/playvtx/
│   ├── Makefile.am          [EDITED] conditional LDFLAGS for CoreAudio
│   └── playvtx.c            [EDITED] +~100 lines, all behind #ifdef __APPLE__
└── docs/superpowers/specs/  [NEW] this design doc
```

No new source files. No audio-backend abstraction layer (YAGNI — two backends
is not enough to justify an interface).

## CoreAudio backend (in `playvtx.c`)

### API choice

**AudioQueue Services** (`<AudioToolbox/AudioToolbox.h>`) — high-level, push
model. Emulator runs on the main thread, CoreAudio pulls via callback on its
own thread. No real-time constraints — the existing `ayemu_gen_sound` path is
not designed for a realtime callback (allocates, uses stdio for warnings).

### Format

Fixed, no negotiation. Matches existing `playvtx.c` globals exactly:

```c
AudioStreamBasicDescription asbd = {0};
asbd.mSampleRate       = 44100;
asbd.mFormatID         = kAudioFormatLinearPCM;
asbd.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
asbd.mFramesPerPacket  = 1;
asbd.mChannelsPerFrame = 2;
asbd.mBitsPerChannel   = 16;
asbd.mBytesPerFrame    = 4;
asbd.mBytesPerPacket   = 4;
```

Native little-endian is correct on both arm64 and x86_64 macOS; no big-endian
flag needed.

### Buffering

Three `AudioQueue` buffers, each sized for one VTX player frame of PCM:

```
bytes_per_buffer = asbd.mBytesPerFrame * (asbd.mSampleRate / vtx->playerFreq)
                 = 4 * (44100 / 50) = 3528  (for a 50 Hz player)
```

One VTX frame per buffer mirrors the existing OSS loop exactly — the
emulator already renders audio in per-frame chunks.

### Context struct

File-scope, one instance per song (reallocated per file):

```c
typedef struct {
    ayemu_vtx_t        *vtx;
    size_t              frame_pos;   // next frame index to render
    dispatch_semaphore_t done;       // signaled when last buffer finishes
    int                 inflight;    // buffers currently enqueued
} ca_ctx_t;
```

### Callback

Runs on CoreAudio's internal thread:

1. If `frame_pos < vtx->frames`:
   - `ayemu_vtx_getframe(vtx, frame_pos++, regs)`
   - `ayemu_set_regs(&ay, regs)`
   - `ayemu_gen_sound(&ay, buf->mAudioData, buf->mAudioDataByteSize)`
   - `AudioQueueEnqueueBuffer(q, buf, 0, NULL)`
2. Else:
   - `inflight--`
   - If `inflight == 0`, `dispatch_semaphore_signal(ctx->done)`
   - Do **not** re-enqueue.

No locks. The main thread only waits on `ctx->done` during playback, so
the callback has exclusive access to `ay`, `regs`, and the VTX struct.

### Main-thread flow (`play_coreaudio(filename)`)

1. Load VTX, reset emulator, set chip type / stereo / frequency (same as
   existing `play()` up to the OSS-specific bits).
2. If `vtx->frames == 0`, free and return — nothing to play.
3. Compute `n_buffers = min(3, vtx->frames)`.
4. `AudioQueueNewOutput(&asbd, callback, &ctx, NULL, NULL, 0, &q)`.
5. `AudioQueueAllocateBuffer(q, bytes_per_buffer, &buf[i])` × `n_buffers`.
6. **Prime**: for each allocated buffer, render one frame and enqueue
   directly (without going through the callback). Set `ctx.inflight = n_buffers`
   and `ctx.frame_pos = n_buffers` after priming.
7. `AudioQueueStart(q, NULL)`.
8. `dispatch_semaphore_wait(ctx.done, DISPATCH_TIME_FOREVER)`.
9. `AudioQueueStop(q, true)`.
10. `AudioQueueDispose(q, true)`.
11. Free VTX.

Priming inline (rather than calling the callback) avoids a bookkeeping
edge case: the callback's "done" path decrements `inflight`, which is not
meaningful during priming.

### Edge cases

- **VTX with fewer than 3 frames:** `n_buffers` is clamped to `vtx->frames`,
  so priming enqueues exactly that many buffers. The callback's "else"
  branch then fires `n_buffers` times (once per buffer finishing) and the
  semaphore signals when `inflight` reaches 0.
- **VTX with zero frames:** handled at step 2 — return before touching the
  queue.
- **Multiple files on the command line:** each file fully plays and disposes
  its queue before the next is loaded. No state leaks.

### Error handling

Any `OSStatus != noErr` from a CoreAudio call (`AudioQueueNewOutput`,
`AudioQueueAllocateBuffer`, `AudioQueueEnqueueBuffer`, `AudioQueueStart`)
prints `"CoreAudio error <call>: <code>"` to stderr and calls `exit(1)`.
Matches the existing OSS code's print-and-die style. No partial recovery,
no fallback to stdout.

### `main()` dispatch

```c
#ifndef __APPLE__
    if (!sflag) init_oss();  /* CoreAudio setup is per-file, no shared init */
#endif

for (index = optind; index < argc; index++) {
    printf("\nPlaying file %s\n", argv[index]);
#ifdef __APPLE__
    if (!sflag) play_coreaudio(argv[index]);
    else        play(argv[index]);
#else
    play(argv[index]);
#endif
}
```

The existing `play()` function stays unchanged. It remains the implementation
for `-s`/stdout mode on all platforms, and for non-stdout mode on non-Apple
systems.

## Autotools changes

### `configure.ac` (+~10 lines)

```m4
AC_CANONICAL_HOST
case "$host_os" in
  darwin*) host_is_darwin=yes ;;
  *)       host_is_darwin=no  ;;
esac
AM_CONDITIONAL([HOST_DARWIN], [test "x$host_is_darwin" = "xyes"])
```

`AC_CANONICAL_HOST` requires `config.guess` and `config.sub`, which are
pulled in by `automake --add-missing --copy` during bootstrap. No extra
action needed.

No `AC_CHECK_HEADERS` for CoreAudio — the headers ship with Xcode Command
Line Tools, which is a hard prerequisite for any C toolchain on macOS.
Checking adds zero safety.

### `apps/playvtx/Makefile.am` (+3 lines)

```make
if HOST_DARWIN
playvtx_LDFLAGS = -framework CoreAudio -framework AudioToolbox
endif
```

### `Makefile.am` (root) and `test/Makefile.am`

No changes. Tests are already disableable via `--disable-tests` (recent
upstream commit `490778b`), which `Makefile.macos` will pass.

## `Makefile.macos` wrapper

POSIX make, top-level file. Hides the autotools dance behind familiar
targets. Invoked as `make -f Makefile.macos <target>`. Cannot be named
plain `Makefile` because `./configure` generates one at the root.

```make
# Convenience wrapper for building libayemu-mac on macOS.
# For the canonical autotools flow, see INSTALL.

PREFIX ?= /opt/homebrew
CONFIGURE_FLAGS ?= --disable-tests --prefix=$(PREFIX)

.PHONY: all bootstrap configure build install uninstall clean distclean help

all: build

bootstrap:
	glibtoolize
	aclocal
	autoconf
	autoheader
	automake --add-missing --include-deps --copy

configure: bootstrap
	./configure $(CONFIGURE_FLAGS)

build: configure
	$(MAKE)

install: build
	sudo $(MAKE) install

uninstall:
	sudo $(MAKE) uninstall

clean:
	-$(MAKE) clean

distclean:
	-$(MAKE) distclean
	rm -f aclocal.m4 config.guess config.sub config.h.in configure \
	      depcomp install-sh ltmain.sh missing compile
	rm -rf autom4te.cache

help:
	@echo "Targets: all bootstrap configure build install uninstall clean distclean"
	@echo "Vars: PREFIX (default $(PREFIX)), CONFIGURE_FLAGS (default $(CONFIGURE_FLAGS))"
```

Notes:

- Uses `glibtoolize` directly (macOS name for `libtoolize`), sidestepping
  the `bootstrap` script's hard reference to `libtoolize`.
- Default `PREFIX=/opt/homebrew` (Apple Silicon Homebrew). Intel Homebrew
  users override: `make -f Makefile.macos PREFIX=/usr/local install`.
- `--disable-tests` default because `test/test.c` still needs OSS.
- `distclean` scrubs autotools-generated files.
- Each target depends on the previous, so `make -f Makefile.macos install`
  runs the whole flow end-to-end.

## `README.md`

Trimmed to macOS focus. Keep a prominent upstream pointer.

Structure:

```markdown
# AY/YM sound co-processor emulation library — macOS port

This is a macOS fork of [asashnov/libayemu](https://github.com/asashnov/libayemu)
adding native CoreAudio output to `playvtx` so `.vtx` files play directly on
macOS without OSS wrappers.

## About

<brief: AY/YM chip, used in ZX Spectrum, VTX file format, player>

## macOS install

    xcode-select --install
    brew install autoconf automake libtool
    make -f Makefile.macos install

## Play

    playvtx music_sample/ritm-4.vtx

## Upstream

The underlying library works on Linux too. See the
[upstream project](https://github.com/asashnov/libayemu) for non-macOS
builds, XMMS and GStreamer plugins, and historical context.

## License

GNU GPL v2 (see COPYING).
```

Drops from the current README: xmms-vtx / gstreamer-vtx discussion, the
cvs2cl / sourceforge links, the "thousands of songs on the Internet"
preamble, and the long external-links section (keep at most 2–3 essentials
if any).

## Fork/publish workflow

Executed once at the end, after implementation is verified locally.

```sh
# 1. Fork upstream under your account (does not clone)
gh repo fork asashnov/libayemu --clone=false

# 2. Rename the fork on GitHub
gh repo rename libayemu-mac --repo pinebit/libayemu

# 3. Repoint local origin to the renamed fork
git remote set-url origin git@github.com:pinebit/libayemu-mac.git

# 4. Push the macOS work on a feature branch, then PR into your master
git checkout -b macos-coreaudio
# ... (implementation commits will already exist at this point)
git push -u origin macos-coreaudio
gh pr create --base master --head macos-coreaudio --title "macOS CoreAudio port"
# review the PR on pinebit/libayemu-mac, merge
```

Why a feature branch + self-PR rather than pushing to master directly:
keeps the macOS changes as a reviewable diff in one place, and leaves a
clean commit on master that's easy to cherry-pick onto upstream later.

## Testing

Manual verification. No automated tests — audio I/O on a specific OS.

Checklist for the implementor:

1. `make -f Makefile.macos distclean && make -f Makefile.macos build` from a
   clean checkout succeeds on the target Mac.
2. `./apps/playvtx/playvtx music_sample/ritm-4.vtx` produces audible sound
   that matches a Linux-build reference (or a known online VTX reference).
3. `./apps/playvtx/playvtx -s music_sample/ritm-4.vtx | sox -t raw -r 44100 -e signed -b 16 -c 2 - /tmp/out.wav`
   still works — stdout path unchanged.
4. `./apps/playvtx/playvtx a.vtx b.vtx` plays both in sequence without
   hanging or skipping.
5. If a VTX with fewer than 3 frames is available, it plays without
   crashing (priming edge case).
6. `leaks -atExit -- ./apps/playvtx/playvtx music_sample/ritm-4.vtx`
   reports no leaks attributable to CoreAudio buffers or queue.
7. `sudo make -f Makefile.macos uninstall` removes all installed files.
8. Upstream compatibility: confirm `configure && make` still builds on
   Linux (Docker image or remote box). The OSS path must compile and link
   unchanged. No CI required — a one-time verification is enough.

## Commits

Implementation will land as a small number of focused commits on a
`macos-coreaudio` branch:

1. `autotools: detect Darwin host, add HOST_DARWIN conditional`
2. `playvtx: add CoreAudio AudioQueue backend for macOS`
3. `build: add Makefile.macos wrapper for one-command macOS build`
4. `docs: retarget README for the macOS fork`

Commit messages follow the existing repo style (lowercase scope prefix,
short imperative subject).
