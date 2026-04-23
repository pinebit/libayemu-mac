# playvtx macOS CoreAudio Port — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add native CoreAudio AudioQueue output to `playvtx` so `.vtx` files play directly on macOS, while preserving the existing Linux/OSS build path.

**Architecture:** Single-file addition in `apps/playvtx/playvtx.c` guarded by `#ifdef __APPLE__`, plus autotools host detection and a convenience `Makefile.macos` wrapper. No library code changes. No new files under `src/` or `include/`. The fork is published as `pinebit/libayemu-mac` after local verification.

**Tech Stack:** C99, GNU autotools (autoconf/automake/libtool), Apple CoreAudio `AudioToolbox.framework` (AudioQueue Services), GCD (`dispatch_semaphore_t`) for main-thread/callback synchronization.

**Design spec:** `docs/superpowers/specs/2026-04-23-playvtx-macos-coreaudio-design.md`

**Commit history target** (4 commits on branch `macos-coreaudio`):
1. `autotools: detect Darwin host, add HOST_DARWIN conditional`
2. `playvtx: add CoreAudio AudioQueue backend for macOS`
3. `build: add Makefile.macos wrapper for one-command macOS build`
4. `docs: retarget README for the macOS fork`

---

### Task 1: Create feature branch

**Files:** (none yet — just git state)

- [ ] **Step 1: Verify clean state**

Run: `git status`
Expected:
```
On branch master
Your branch is up to date with 'origin/master'.
nothing to commit, working tree clean
```
(The `docs/superpowers/` commit from brainstorming should already be in history.)

- [ ] **Step 2: Create and switch to feature branch**

Run: `git checkout -b macos-coreaudio`
Expected: `Switched to a new branch 'macos-coreaudio'`

- [ ] **Step 3: Verify branch**

Run: `git branch --show-current`
Expected: `macos-coreaudio`

---

### Task 2: Install macOS build prerequisites (one-time)

**Files:** (none — environment setup)

- [ ] **Step 1: Install GNU autotools via Homebrew**

Run: `brew install autoconf automake libtool`
Expected: either fresh install output or `Warning: autoconf X is already installed` for each. All three must be present.

- [ ] **Step 2: Verify `glibtoolize` is on PATH**

Run: `which glibtoolize && glibtoolize --version | head -1`
Expected output starts with a path under `/opt/homebrew/bin/` (or `/usr/local/bin/` on Intel Macs) and a version like `libtoolize (GNU libtool) 2.x.x`.

- [ ] **Step 3: Verify Xcode Command Line Tools are installed (CoreAudio headers)**

Run: `xcrun --find clang && ls $(xcrun --show-sdk-path)/System/Library/Frameworks/AudioToolbox.framework/Headers/AudioQueue.h`
Expected: both commands succeed. If the second path is missing, run `xcode-select --install` and retry.

---

### Task 3: Autotools — detect Darwin host, link CoreAudio frameworks

**Files:**
- Modify: `configure.ac`
- Modify: `apps/playvtx/Makefile.am`

- [ ] **Step 1: Edit `configure.ac` — add host detection**

Open `configure.ac`. Between the `AM_INIT_AUTOMAKE` line (line 4) and the `# Options` comment (line 6), insert:

```m4
AC_CANONICAL_HOST
case "$host_os" in
  darwin*) host_is_darwin=yes ;;
  *)       host_is_darwin=no  ;;
esac
AM_CONDITIONAL([HOST_DARWIN], [test "x$host_is_darwin" = "xyes"])
```

The file should look like:
```m4
AC_INIT([libayemu], [1.1.0])
AM_CONFIG_HEADER(config.h)
AM_INIT_AUTOMAKE

AC_CANONICAL_HOST
case "$host_os" in
  darwin*) host_is_darwin=yes ;;
  *)       host_is_darwin=no  ;;
esac
AM_CONDITIONAL([HOST_DARWIN], [test "x$host_is_darwin" = "xyes"])

# Options
# AC_ARG_ENABLE
...
```

- [ ] **Step 2: Edit `apps/playvtx/Makefile.am` — add conditional LDFLAGS**

Current file:
```make

AM_CPPFLAGS = -I$(top_srcdir)/include

bin_PROGRAMS        = playvtx
playvtx_SOURCES     = playvtx.c
playvtx_LDADD       = $(top_srcdir)/src/libayemu.la
```

Append at the end:
```make

if HOST_DARWIN
playvtx_LDFLAGS = -framework CoreAudio -framework AudioToolbox
endif
```

- [ ] **Step 3: Bootstrap the autotools files (first time on this branch)**

Run, from the repo root:
```bash
glibtoolize
aclocal
autoconf
autoheader
automake --add-missing --include-deps --copy
```
Expected: no errors. Warnings about obsolete macros (`AM_CONFIG_HEADER`, `AC_PROG_LIBTOOL`) are pre-existing and fine. Files `config.guess`, `config.sub`, `configure`, `Makefile.in`, etc. now exist.

- [ ] **Step 4: Configure with tests disabled (OSS-dependent)**

Run: `./configure --disable-tests`
Expected: ends with `config.status: creating config.h` and no errors. The line `checking host system type...` should appear and resolve to e.g. `arm64-apple-darwin24.x.x` or `x86_64-apple-darwin...`.

- [ ] **Step 5: Verify `HOST_DARWIN` propagated to the playvtx Makefile**

Run: `grep -E '^HOST_DARWIN_(TRUE|FALSE)' apps/playvtx/Makefile`
Expected (on macOS):
```
HOST_DARWIN_TRUE = 
HOST_DARWIN_FALSE = #
```
(`_TRUE` empty = condition active, `_FALSE` commented out = inactive).

- [ ] **Step 6: Verify CoreAudio LDFLAGS appear in the playvtx Makefile**

Run: `grep -- '-framework' apps/playvtx/Makefile`
Expected: a line containing `-framework CoreAudio -framework AudioToolbox`.

Note: running `make` at this point will **fail** on macOS because `playvtx.c` still unconditionally includes `<sys/soundcard.h>`. That's fixed in Task 4. Do not run `make` yet.

- [ ] **Step 7: Commit**

```bash
git add configure.ac apps/playvtx/Makefile.am
git commit -m "$(cat <<'EOF'
autotools: detect Darwin host, add HOST_DARWIN conditional

AC_CANONICAL_HOST + case on $host_os sets HOST_DARWIN. When set, link
playvtx against CoreAudio.framework and AudioToolbox.framework. This
prepares the build system for the macOS CoreAudio backend in the
following commit; no behaviour change on Linux.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```
Expected: commit created. `git log --oneline -1` shows the new commit.

---

### Task 4: playvtx — add CoreAudio AudioQueue backend

**Files:**
- Modify: `apps/playvtx/playvtx.c`

This is the meat of the port. We (a) guard the OSS code behind `#if !defined(__APPLE__)`, (b) add a self-contained CoreAudio block behind `#ifdef __APPLE__`, and (c) dispatch in `main()`.

- [ ] **Step 1: Guard the OSS include**

In `apps/playvtx/playvtx.c`, replace the line:
```c
#include <sys/soundcard.h>
```
with:
```c
#if !defined(__APPLE__)
#include <sys/soundcard.h>
#endif
```

- [ ] **Step 2: Guard the `init_oss()` function definition**

Wrap the entire `void init_oss()` function (currently roughly lines 90–118 — starts at `void init_oss()` and ends at the closing `}` of that function) between:
```c
#if !defined(__APPLE__)
void init_oss()
{
  /* ... existing body unchanged ... */
}
#endif
```

Exact replacement — find this block:
```c
void init_oss()
{
  if ((audio_fd = open(DEVICE_NAME, O_WRONLY, 0)) == -1) {
    fprintf (stderr,
        "Unable to initialize OSS sound system: unable to open /dev/dsp\n"
        "\n"
        "Probably you are running a modern Linux with ALSA or PulseAudio.\n"
        "\n"
        "On systems with PulseAudio, such as Ubuntu, run with:\n"
        "  $ padsp playvtx music_sample/secret.vtx \n"
        "\n"
        "On systems with ALSA use alsa-oss wrapper:\n"
        "  $ aoss playvtx music_sample/secret.vtx \n"
        );
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_SETFMT, &bits) == -1) {
    fprintf (stderr, "Can't set sound format\n");
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_CHANNELS, &chans) == -1) {
    fprintf (stderr, "Can't set number of channels\n");
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_SPEED, &freq) == -1) {
    fprintf (stderr, "Can't set audio freq\n");
  }
  else
    return;

  exit(1);
}
```
and replace with (same content, now wrapped):
```c
#if !defined(__APPLE__)
void init_oss()
{
  if ((audio_fd = open(DEVICE_NAME, O_WRONLY, 0)) == -1) {
    fprintf (stderr,
        "Unable to initialize OSS sound system: unable to open /dev/dsp\n"
        "\n"
        "Probably you are running a modern Linux with ALSA or PulseAudio.\n"
        "\n"
        "On systems with PulseAudio, such as Ubuntu, run with:\n"
        "  $ padsp playvtx music_sample/secret.vtx \n"
        "\n"
        "On systems with ALSA use alsa-oss wrapper:\n"
        "  $ aoss playvtx music_sample/secret.vtx \n"
        );
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_SETFMT, &bits) == -1) {
    fprintf (stderr, "Can't set sound format\n");
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_CHANNELS, &chans) == -1) {
    fprintf (stderr, "Can't set number of channels\n");
  }
  else if (ioctl(audio_fd, SNDCTL_DSP_SPEED, &freq) == -1) {
    fprintf (stderr, "Can't set audio freq\n");
  }
  else
    return;

  exit(1);
}
#endif /* !__APPLE__ */
```

- [ ] **Step 3: Add the CoreAudio block**

Directly **after** the `#endif` from Step 2 (i.e., after the guarded `init_oss()`), insert this complete block:

```c
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>

typedef struct {
    ayemu_vtx_t         *vtx;
    size_t               frame_pos;     /* next frame index to render */
    dispatch_semaphore_t done;          /* signaled when last buffer finishes */
    int                  inflight;      /* buffers currently enqueued */
    int                  bytes_per_buffer;
} ca_ctx_t;

static void ca_die(const char *call, OSStatus status)
{
  fprintf(stderr, "CoreAudio error %s: %d\n", call, (int)status);
  exit(1);
}

static void ca_fill_buffer(ca_ctx_t *ctx, AudioQueueBufferRef buf)
{
  ayemu_vtx_getframe(ctx->vtx, ctx->frame_pos++, regs);
  ayemu_set_regs(&ay, regs);
  ayemu_gen_sound(&ay, buf->mAudioData, ctx->bytes_per_buffer);
  buf->mAudioDataByteSize = ctx->bytes_per_buffer;
}

static void ca_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
  ca_ctx_t *ctx = (ca_ctx_t *)user;
  if (ctx->frame_pos < ctx->vtx->frames) {
    ca_fill_buffer(ctx, buf);
    OSStatus s = AudioQueueEnqueueBuffer(q, buf, 0, NULL);
    if (s != noErr) ca_die("AudioQueueEnqueueBuffer (callback)", s);
  } else {
    if (--ctx->inflight == 0) {
      dispatch_semaphore_signal(ctx->done);
    }
  }
}

static void play_coreaudio(const char *filename)
{
  ayemu_vtx_t *vtx;
  AudioQueueRef q;
  AudioQueueBufferRef bufs[3];
  ca_ctx_t ctx;
  AudioStreamBasicDescription asbd;
  OSStatus s;
  int n_buffers;
  int i;

  vtx = ayemu_vtx_load_from_file(filename);
  if (!vtx) return;

  if (!qflag)
    printf(" Title: %s\n Author: %s\n From: %s\n Comment: %s\n Year: %d\n",
           vtx->title, vtx->author, vtx->from, vtx->comment, vtx->year);

  if (vtx->frames == 0) {
    ayemu_vtx_free(vtx);
    return;
  }

  ayemu_reset(&ay);
  ayemu_set_chip_type(&ay, vtx->chiptype, NULL);
  ayemu_set_stereo(&ay, vtx->stereo, NULL);
  if (!Tflag)
    ayemu_set_chip_freq(&ay, vtx->chipFreq);
  else
    ayemu_set_chip_freq(&ay, 3500000);

  memset(&asbd, 0, sizeof(asbd));
  asbd.mSampleRate       = freq;
  asbd.mFormatID         = kAudioFormatLinearPCM;
  asbd.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  asbd.mFramesPerPacket  = 1;
  asbd.mChannelsPerFrame = chans;
  asbd.mBitsPerChannel   = bits;
  asbd.mBytesPerFrame    = chans * (bits >> 3);
  asbd.mBytesPerPacket   = asbd.mBytesPerFrame;

  memset(&ctx, 0, sizeof(ctx));
  ctx.vtx              = vtx;
  ctx.frame_pos        = 0;
  ctx.done             = dispatch_semaphore_create(0);
  ctx.bytes_per_buffer = asbd.mBytesPerFrame * (freq / vtx->playerFreq);

  n_buffers = (vtx->frames < 3) ? (int)vtx->frames : 3;
  ctx.inflight = n_buffers;

  s = AudioQueueNewOutput(&asbd, ca_callback, &ctx, NULL, NULL, 0, &q);
  if (s != noErr) ca_die("AudioQueueNewOutput", s);

  for (i = 0; i < n_buffers; i++) {
    s = AudioQueueAllocateBuffer(q, ctx.bytes_per_buffer, &bufs[i]);
    if (s != noErr) ca_die("AudioQueueAllocateBuffer", s);
    ca_fill_buffer(&ctx, bufs[i]);
    s = AudioQueueEnqueueBuffer(q, bufs[i], 0, NULL);
    if (s != noErr) ca_die("AudioQueueEnqueueBuffer (prime)", s);
  }

  s = AudioQueueStart(q, NULL);
  if (s != noErr) ca_die("AudioQueueStart", s);

  dispatch_semaphore_wait(ctx.done, DISPATCH_TIME_FOREVER);

  AudioQueueStop(q, true);
  AudioQueueDispose(q, true);
  dispatch_release(ctx.done);

  ayemu_vtx_free(vtx);
}
#endif /* __APPLE__ */
```

- [ ] **Step 4: Guard the `init_oss()` call in `main()`**

Find the block in `main()` that currently reads:
```c
  if (! sflag)
    init_oss();
```
Replace with:
```c
#if !defined(__APPLE__)
  if (! sflag)
    init_oss();  /* CoreAudio setup is per-file on macOS, no shared init */
#endif
```

- [ ] **Step 5: Add CoreAudio/stdout dispatch in the main playback loop**

Find the block in `main()` that currently reads:
```c
  for (index = optind; index < argc; index++) {
    printf ("\nPlaying file %s\n", argv[index]);
    play (argv[index]);
  }
```
Replace with:
```c
  for (index = optind; index < argc; index++) {
    printf ("\nPlaying file %s\n", argv[index]);
#ifdef __APPLE__
    if (!sflag) play_coreaudio(argv[index]);
    else        play(argv[index]);
#else
    play(argv[index]);
#endif
  }
```

- [ ] **Step 6: Build and verify the binary produced**

Run: `make`
Expected: builds successfully; final link line contains `-framework CoreAudio -framework AudioToolbox`. No warnings about missing headers.

- [ ] **Step 7: Verify CoreAudio frameworks are linked into the binary**

Run: `otool -L apps/playvtx/playvtx | grep -iE 'coreaudio|audiotoolbox'`
Expected: two lines, one for `CoreAudio.framework/Versions/A/CoreAudio` and one for `AudioToolbox.framework/Versions/A/AudioToolbox`.

- [ ] **Step 8: Verify --stdout path still works (unchanged behaviour)**

Run: `./apps/playvtx/playvtx -s music_sample/ritm-4.vtx | head -c 128 | xxd | head -4`
Expected: hex dump of non-zero PCM bytes (not all 00s).

- [ ] **Step 9: Verify audible playback of a single file**

Run: `./apps/playvtx/playvtx music_sample/ritm-4.vtx`

Expected: track title/author/etc. printed to stdout; music plays through the system default output; the program exits cleanly when the track ends (`$?` = 0).

This is a manual check — you must actually hear music. If no sound plays but the program exits cleanly, check:
- System audio output is not muted / routed to an absent device
- `otool -L` output from Step 7 shows the frameworks
- Try a different file (`music_sample/secret.vtx`)

- [ ] **Step 10: Verify sequential multi-file playback**

Run: `./apps/playvtx/playvtx music_sample/secret.vtx music_sample/csoon.vtx`
Expected: both files play in order, no hang between them, both finish audibly. Ctrl+C between files is fine if you don't want to listen to the whole thing.

- [ ] **Step 11: Verify no leaks under `leaks -atExit`**

Run: `MallocStackLogging=1 leaks -atExit -- ./apps/playvtx/playvtx music_sample/ritm-4.vtx 2>&1 | tail -20`

Expected: `0 leaks for 0 total leaked bytes` in the final report, **or** leaks only from CoreAudio system frameworks (common — these are outside our control). Any leak with a stack trace pointing into `playvtx.c` or `ayemu_*` is a real bug — fix before proceeding.

- [ ] **Step 12: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "$(cat <<'EOF'
playvtx: add CoreAudio AudioQueue backend for macOS

On Darwin, playvtx now plays .vtx files through the system default
output via AudioToolbox's AudioQueue Services. Three queue buffers,
push model, dispatch_semaphore for end-of-track synchronisation.
The OSS path is preserved verbatim behind #if !defined(__APPLE__);
the --stdout path is unchanged on all platforms.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 5: Add `Makefile.macos` wrapper

**Files:**
- Create: `Makefile.macos`

- [ ] **Step 1: Create `Makefile.macos` at the repo root**

Write the following content to `Makefile.macos`:

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

Important: the recipe lines **must** begin with a literal TAB character, not spaces. POSIX make will reject space-indented recipes with `missing separator` or `commands commence before first target`.

- [ ] **Step 2: Verify `help` target prints targets**

Run: `make -f Makefile.macos help`
Expected output:
```
Targets: all bootstrap configure build install uninstall clean distclean
Vars: PREFIX (default /opt/homebrew), CONFIGURE_FLAGS (default --disable-tests --prefix=/opt/homebrew)
```

- [ ] **Step 3: End-to-end clean build via the wrapper**

Run:
```bash
make -f Makefile.macos distclean
make -f Makefile.macos build
```

Expected: `distclean` removes configure/Makefile/autotools artefacts (some errors from `make distclean` on already-clean state are fine, caught by the leading `-` in the recipe). `build` runs glibtoolize/aclocal/autoconf/autoheader/automake then `./configure` then `make` — each step visible in the output. Final artefact: `apps/playvtx/playvtx`.

- [ ] **Step 4: Verify the binary built by the wrapper also runs**

Run: `./apps/playvtx/playvtx music_sample/ritm-4.vtx`
Expected: audible playback, same as Task 4 Step 9.

- [ ] **Step 5: Commit**

```bash
git add Makefile.macos
git commit -m "$(cat <<'EOF'
build: add Makefile.macos wrapper for one-command macOS build

Top-level convenience wrapper hiding the glibtoolize → aclocal →
autoconf → autoheader → automake → configure → make sequence behind
a familiar make target set (all, install, clean, distclean, etc.).
Defaults to PREFIX=/opt/homebrew and --disable-tests since the tests
are still OSS-only.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 6: Retarget `README.md` to the macOS fork

**Files:**
- Modify: `README.md` (rewrite)

- [ ] **Step 1: Rewrite `README.md`**

Replace the entire contents of `README.md` with:

```markdown
# AY/YM sound co-processor emulation library — macOS port

This is a macOS fork of [asashnov/libayemu](https://github.com/asashnov/libayemu)
adding native CoreAudio output to `playvtx` so `.vtx` files play directly on
macOS without OSS wrappers.

## About

The AY-3-8910 / YM2149 sound chip was used in the Sinclair ZX Spectrum 128K,
Atari ST and several other home computers of the 1980s. This library emulates
it, and `playvtx` plays songs in the `.vtx` file format — a compact register
dump used widely in the ZX Spectrum music scene.

## macOS install

One-time prerequisites:

    xcode-select --install
    brew install autoconf automake libtool

Build and install:

    make -f Makefile.macos install

Remove:

    make -f Makefile.macos uninstall

The default install prefix is `/opt/homebrew` (Apple Silicon Homebrew).
On Intel Macs with Homebrew under `/usr/local`, run:

    make -f Makefile.macos PREFIX=/usr/local install

## Play

    playvtx music_sample/ritm-4.vtx

Multiple files play in sequence:

    playvtx music_sample/*.vtx

Dump raw PCM to stdout (for piping to `sox`, `ffmpeg`, etc.):

    playvtx -s music_sample/secret.vtx | sox -t raw -r 44100 -e signed -b 16 -c 2 - secret.wav

## Upstream

The underlying library builds on Linux too. The upstream project ships
XMMS and GStreamer plugins and has historical context about the VTX
format. See [asashnov/libayemu](https://github.com/asashnov/libayemu).

## License

GNU GPL v2. See `COPYING`.
```

- [ ] **Step 2: Spot-check the result renders**

Run: `head -30 README.md`
Expected: clean markdown starting with the H1 heading, no stray characters from the rewrite.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "$(cat <<'EOF'
docs: retarget README for the macOS fork

Trim to a macOS-focused install/play story. Upstream project linked
prominently for Linux builds, XMMS/GStreamer plugins, and historical
context.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

### Task 7: Verify Linux/OSS build path still compiles (optional cross-check)

**Files:** (none — cross-platform validation)

The spec calls for confirming the OSS path on Linux is unchanged. This is optional because the macOS changes are strictly additive behind `#ifdef __APPLE__` / `#if !defined(__APPLE__)` — but it's cheap to run and catches one category of bug (e.g., an accidentally-unguarded CoreAudio reference).

Pick whichever of the two sub-paths is easier for you:

**Sub-path A: Docker (recommended if Docker is installed)**

- [ ] **Step 1: Build in a Debian container**

Run:
```bash
docker run --rm -v "$PWD":/src -w /src debian:stable bash -c '
  apt-get update -qq && \
  apt-get install -y -qq autoconf automake libtool build-essential && \
  ./bootstrap && \
  ./configure && \
  make 2>&1 | tail -20
'
```

Expected: `make` completes without error. The tail output shows linking of `libayemu.la`, `test/test`, and `apps/playvtx/playvtx`. No references to CoreAudio or AudioToolbox.

If `./bootstrap` complains about dirty autotools state from the macOS build, first run `make -f Makefile.macos distclean` on the host before the Docker command.

**Sub-path B: Remote Linux machine**

- [ ] **Step 1: Push to a Linux box you control, run the standard upstream flow**

```bash
rsync -az --exclude=.git --exclude=autom4te.cache ./ user@linuxbox:/tmp/libayemu-mac/
ssh user@linuxbox 'cd /tmp/libayemu-mac && ./bootstrap && ./configure && make 2>&1 | tail -20'
```

Expected: same as Sub-path A.

**Sub-path C: Skip**

If neither A nor B is available, skip this task and rely on the `#ifdef` isolation guarantee. Re-verify later if you get reports from Linux users of the fork.

(No commit — this is verification only.)

---

### Task 8: Publish — fork upstream, rename, push branch, self-PR

**Files:** (none — GitHub/remote operations)

**This is a visible external action. Do not run these steps until Tasks 1–6 are complete and verified locally.** Confirm with the user before each `gh` command that performs a mutation.

- [ ] **Step 1: Fork upstream under your GitHub account**

Run: `gh repo fork asashnov/libayemu --clone=false`
Expected: `✓ Created fork pinebit/libayemu`

If a repo named `libayemu` already exists under `pinebit`, this will fail — resolve that first. `gh repo list pinebit --limit 200 | grep libayemu` shows what's there.

- [ ] **Step 2: Rename the fork on GitHub**

Run: `gh repo rename libayemu-mac --repo pinebit/libayemu --yes`
Expected: `✓ Renamed repository to pinebit/libayemu-mac`

- [ ] **Step 3: Repoint the local `origin` remote**

Run:
```bash
git remote set-url origin git@github.com:pinebit/libayemu-mac.git
git remote -v
```
Expected: both `fetch` and `push` URLs now point at `pinebit/libayemu-mac`.

- [ ] **Step 4: Push the `master` branch (to inherit upstream history on the fork)**

Run: `git push origin master`
Expected: `Everything up-to-date` (the fork already has the upstream history) or successful push of any new `master` commits (like the spec/plan commits).

- [ ] **Step 5: Push the feature branch**

Run: `git push -u origin macos-coreaudio`
Expected: new remote branch `macos-coreaudio` created, tracking set up.

- [ ] **Step 6: Create self-PR on the fork**

Run:
```bash
gh pr create \
  --repo pinebit/libayemu-mac \
  --base master \
  --head macos-coreaudio \
  --title "macOS CoreAudio port" \
  --body "$(cat <<'EOF'
## Summary

- Adds native CoreAudio (AudioQueue) output to `playvtx` on macOS so `.vtx` files play directly through the system default output with no OSS wrappers.
- Preserves the Linux/OSS build path verbatim behind `#if !defined(__APPLE__)`.
- Adds a `Makefile.macos` wrapper that hides the glibtoolize → autoconf → automake → configure → make sequence.
- Retargets `README.md` to the macOS fork.

Design: `docs/superpowers/specs/2026-04-23-playvtx-macos-coreaudio-design.md`
Plan: `docs/superpowers/plans/2026-04-23-playvtx-macos-coreaudio-port.md`

## Test plan

- [x] `make -f Makefile.macos distclean && make -f Makefile.macos build` succeeds from clean
- [x] `playvtx music_sample/ritm-4.vtx` plays audibly
- [x] `playvtx -s` stdout path still produces PCM bytes
- [x] Multi-file sequence plays without hang
- [x] `leaks -atExit` shows no leaks in `playvtx.c` / `ayemu_*`
- [x] Linux OSS path still builds (cross-check via Docker or remote)

🤖 Generated with [Claude Code](https://claude.com/claude-code)
EOF
)"
```
Expected: PR URL printed. Open it in a browser for visual review.

- [ ] **Step 7: Merge the PR**

Run: `gh pr merge --repo pinebit/libayemu-mac --merge --delete-branch`
Expected: `✓ Merged pull request #1`, remote branch deleted. `git pull --ff-only origin master` on the local side catches master up.

---

## Self-Review

**Spec coverage** — mapped each section of the design spec to a task:
- Goal / simplest UX → Task 5 + Task 6 (wrapper + README).
- Non-goals → acknowledged by *absence* (no SIGINT, device selection, Homebrew tap, CI).
- Architecture + file list → Task 3 (autotools), Task 4 (playvtx), Task 5 (Makefile.macos), Task 6 (README).
- CoreAudio backend (API, format, buffering, context, callback, main flow, edge cases, error handling, main dispatch) → Task 4 Steps 1–5.
- Autotools changes → Task 3 Steps 1–2.
- Makefile.macos wrapper → Task 5 Step 1.
- README → Task 6 Step 1.
- Fork/publish workflow → Task 8.
- Testing (all 8 checklist items from the spec) → Task 3 Steps 5–6, Task 4 Steps 7–11, Task 5 Steps 3–4, Task 7, Task 8.
- Commits (4 commits) → Task 3 Step 7, Task 4 Step 12, Task 5 Step 5, Task 6 Step 3.

**Placeholder scan:** no "TBD" / "TODO" / "implement later" / "similar to above" patterns in the plan. All code blocks are complete and copy-pasteable.

**Type consistency:** `ca_ctx_t` is defined in Task 4 Step 3 and referenced consistently thereafter. Function names `ca_die`, `ca_fill_buffer`, `ca_callback`, `play_coreaudio` match between declaration, definition, and call sites. `n_buffers` bound matches the `bufs[3]` stack array (clamped via `min(3, vtx->frames)`).

**One consciously-accepted intermediate state:** After Task 3 commits, a `make` on macOS at that commit fails because OSS headers aren't yet guarded. This is resolved in Task 4 and noted inline. Acceptable because we control the commit sequence; never shipped to anyone until Task 8.
