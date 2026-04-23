# Esc-to-Stop Playback in playvtx Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Press Esc during interactive playback in playvtx to exit the player immediately, on both the CoreAudio and OSS paths.

**Architecture:** Add terminal raw mode so Esc is delivered without waiting for Enter; a non-blocking `poll_esc()` helper sets a shared `stop_requested` flag; each playback path checks the flag per frame (or per 10ms timeout for CoreAudio). Terminal state is restored via `atexit()`.

**Tech Stack:** C, POSIX termios, POSIX fcntl, CoreAudio AudioQueue (macOS), autotools build (`make` from repo root).

---

## File map

| File | Change |
|------|--------|
| `apps/playvtx/playvtx.c` | Add `<termios.h>` include, raw-mode functions, `stop_requested` flag, `poll_esc()`, update `play()`, `ca_callback()`, `play_coreaudio()` wait loop, and `main()` playlist loop |

---

### Task 1: Add terminal raw mode infrastructure and stop flag

**Files:**
- Modify: `apps/playvtx/playvtx.c`

- [ ] **Step 1: Add `<termios.h>` include**

In `apps/playvtx/playvtx.c`, add `#include <termios.h>` after the existing `#include <fcntl.h>` line (line 28):

```c
#include <fcntl.h>
#include <termios.h>
```

- [ ] **Step 2: Add `stop_requested` flag and terminal state globals**

After the existing `int vflag = 0;  // verbose` line (around line 58), add:

```c
static volatile int stop_requested = 0;

static struct termios saved_termios;
static int term_is_raw = 0;
```

- [ ] **Step 3: Add `term_restore()` and `term_raw_enable()` functions**

After the `usage()` function (after its closing `}`), add:

```c
static void term_restore(void)
{
  if (!term_is_raw) return;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
  int flags = fcntl(STDIN_FILENO, F_GETFL);
  fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
  term_is_raw = 0;
}

static void term_raw_enable(void)
{
  struct termios raw;
  tcgetattr(STDIN_FILENO, &saved_termios);
  atexit(term_restore);
  raw = saved_termios;
  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN]  = 0;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  int flags = fcntl(STDIN_FILENO, F_GETFL);
  fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  term_is_raw = 1;
}
```

- [ ] **Step 4: Add `poll_esc()` helper**

Immediately after `term_raw_enable()`, add:

```c
static void poll_esc(void)
{
  if (!term_is_raw) return;
  char c;
  if (read(STDIN_FILENO, &c, 1) == 1 && c == 0x1B)
    stop_requested = 1;
}
```

The `term_is_raw` guard prevents a blocking `read()` on stdin when `--stdout` is active and raw mode was never enabled.

- [ ] **Step 5: Build to verify no compilation errors**

```bash
make
```

Expected: build succeeds, no errors or warnings about new symbols.

- [ ] **Step 6: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: add terminal raw mode and Esc detection infrastructure"
```

---

### Task 2: Wire `term_raw_enable()` into `main()`

**Files:**
- Modify: `apps/playvtx/playvtx.c` — `main()` function

- [ ] **Step 1: Call `term_raw_enable()` before the playback loop**

In `main()`, locate the existing block (around line 343):

```c
#if !defined(__APPLE__)
  if (! sflag)
    init_oss();  /* CoreAudio setup is per-file on macOS, no shared init */
#endif
```

Add the `term_raw_enable()` call immediately after it:

```c
#if !defined(__APPLE__)
  if (! sflag)
    init_oss();  /* CoreAudio setup is per-file on macOS, no shared init */
#endif

  if (!sflag)
    term_raw_enable();
```

- [ ] **Step 2: Build to verify**

```bash
make
```

Expected: build succeeds.

- [ ] **Step 3: Smoke test — terminal restores cleanly**

```bash
./apps/playvtx/playvtx music_sample/secret.vtx
```

Play briefly, let it finish naturally. Verify the terminal is usable afterward (cursor visible, Enter works, typed characters echo). If the terminal is broken after exit, `term_restore()` is not running — recheck `atexit()` registration.

- [ ] **Step 4: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: call term_raw_enable in main for interactive playback"
```

---

### Task 3: Update `play()` loop (OSS/stdout path)

**Files:**
- Modify: `apps/playvtx/playvtx.c` — `play()` function

- [ ] **Step 1: Update loop condition and add `poll_esc()` call**

In `play()`, find the for loop (around line 267):

```c
  for (pos = 0; pos < vtx->frames; pos++) {
    ayemu_vtx_getframe (vtx, pos, regs);
```

Replace it with:

```c
  for (pos = 0; pos < vtx->frames && !stop_requested; pos++) {
    poll_esc();
    ayemu_vtx_getframe (vtx, pos, regs);
```

- [ ] **Step 2: Build to verify**

```bash
make
```

Expected: build succeeds.

- [ ] **Step 3: Test Esc stops OSS playback (or stdout path)**

On Linux with OSS, run a file and press Esc. On macOS, test the stdout path:

```bash
./apps/playvtx/playvtx --stdout music_sample/secret.vtx > /dev/null
```

For the stdout path Esc detection is intentionally disabled — verify the file plays to completion without issue.

- [ ] **Step 4: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: stop play() loop on Esc"
```

---

### Task 4: Update CoreAudio `ca_callback` to respect stop flag

**Files:**
- Modify: `apps/playvtx/playvtx.c` — `ca_callback()` function (macOS only)

- [ ] **Step 1: Check `stop_requested` at callback entry**

Find `ca_callback()` (around line 150):

```c
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
```

Replace the `if` condition to also check `stop_requested`:

```c
static void ca_callback(void *user, AudioQueueRef q, AudioQueueBufferRef buf)
{
  ca_ctx_t *ctx = (ca_ctx_t *)user;
  if (ctx->frame_pos < ctx->vtx->frames && !stop_requested) {
    ca_fill_buffer(ctx, buf);
    OSStatus s = AudioQueueEnqueueBuffer(q, buf, 0, NULL);
    if (s != noErr) ca_die("AudioQueueEnqueueBuffer (callback)", s);
  } else {
    if (--ctx->inflight == 0) {
      dispatch_semaphore_signal(ctx->done);
    }
  }
}
```

- [ ] **Step 2: Build to verify**

```bash
make
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: stop CoreAudio callback on Esc"
```

---

### Task 5: Replace CoreAudio semaphore wait with polling loop

**Files:**
- Modify: `apps/playvtx/playvtx.c` — `play_coreaudio()` function (macOS only)

- [ ] **Step 1: Replace `dispatch_semaphore_wait` with polling loop**

In `play_coreaudio()`, find (around line 228):

```c
  dispatch_semaphore_wait(ctx.done, DISPATCH_TIME_FOREVER);
```

Replace with:

```c
  while (!stop_requested &&
         dispatch_semaphore_wait(ctx.done,
             dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0) {
    poll_esc();
  }
```

- [ ] **Step 2: Build to verify**

```bash
make
```

Expected: build succeeds.

- [ ] **Step 3: Test — Esc stops CoreAudio playback**

```bash
./apps/playvtx/playvtx music_sample/secret.vtx
```

Press Esc within the first few seconds. Expected: playback stops immediately (within ~10ms), terminal is restored cleanly, process exits.

- [ ] **Step 4: Test — natural end still works**

```bash
./apps/playvtx/playvtx music_sample/secret.vtx
```

Let it play to completion without pressing anything. Expected: plays fully, exits cleanly, terminal restored.

- [ ] **Step 5: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: replace CoreAudio semaphore wait with Esc-aware polling loop"
```

---

### Task 6: Update `main()` playlist loop and final verification

**Files:**
- Modify: `apps/playvtx/playvtx.c` — `main()` function

- [ ] **Step 1: Break playlist loop when Esc was pressed**

In `main()`, find the playback for-loop (around line 355):

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
    if (stop_requested) break;
  }
```

- [ ] **Step 2: Build final binary**

```bash
make
```

Expected: build succeeds with no warnings.

- [ ] **Step 3: Test Esc during multi-file playlist stops at current file**

```bash
./apps/playvtx/playvtx music_sample/secret.vtx music_sample/secret.vtx
```

Press Esc during the first file. Expected: first file stops immediately, second file does not start, process exits, terminal restored.

- [ ] **Step 4: Test `--stdout` is unaffected**

```bash
./apps/playvtx/playvtx --stdout music_sample/secret.vtx | wc -c
```

Expected: outputs a non-zero byte count (full file rendered), no terminal mode changes.

- [ ] **Step 5: Commit**

```bash
git add apps/playvtx/playvtx.c
git commit -m "playvtx: break playlist loop on Esc"
```
