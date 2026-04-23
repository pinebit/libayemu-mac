# Design: Esc key stops playback in playvtx

Date: 2026-04-23

## Goal

When the user presses Esc during interactive playback, playvtx exits immediately. This applies to both the CoreAudio path (macOS) and the OSS/stdout path. Esc detection is disabled when `--stdout` is active, since there is no interactive terminal in that mode.

## Scope

All changes are confined to `apps/playvtx/playvtx.c`. No new files, no new dependencies, no changes to build system.

## Components

### 1. Terminal raw mode (`term_raw_enable` / `term_restore`)

`term_raw_enable()` saves the current `termios`, disables `ICANON` and `ECHO`, sets `stdin` to `O_NONBLOCK`, and registers `term_restore()` via `atexit()`. This ensures the terminal is restored on any exit path — normal return, `exit(1)`, or early return from `play_coreaudio`.

`term_restore()` unconditionally restores the saved `termios` and clears the `O_NONBLOCK` flag. Safe to call multiple times.

Called from `main()` when `!sflag`.

### 2. Shared stop flag

```c
static volatile int stop_requested = 0;
```

Written only by `poll_esc()`. Read by the OSS loop condition and the CoreAudio callback/wait loop.

### 3. `poll_esc()` helper

```c
static void poll_esc(void) {
    char c;
    if (read(STDIN_FILENO, &c, 1) == 1 && c == 0x1B)
        stop_requested = 1;
}
```

Non-blocking (stdin has `O_NONBLOCK`). Returns immediately. Called once per frame in both playback paths.

### 4. OSS/stdout path (`play`)

Loop condition extended: `pos < vtx->frames && !stop_requested`. `poll_esc()` called at the top of each iteration. No other changes.

### 5. CoreAudio path (`play_coreaudio`)

`ca_callback`: at entry, if `stop_requested` is set, treat as end-of-stream — decrement `inflight`, signal `done` if it reaches zero, return without enqueueing.

`dispatch_semaphore_wait` replaced with a 10ms-timeout polling loop:

```c
while (!stop_requested &&
       dispatch_semaphore_wait(ctx.done,
           dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC)) != 0) {
    poll_esc();
}
```

`AudioQueueStop` and `AudioQueueDispose` run unconditionally after the loop — correct for both natural end and Esc-interrupted playback.

## Error handling

Terminal restore is registered via `atexit()`, so it runs even if `exit(1)` is called deep in CoreAudio error paths (`ca_die`). No additional error handling is needed.

### 6. `main()` playlist loop

The `for` loop over `argv` gains a `stop_requested` break after each file:

```c
for (index = optind; index < argc; index++) {
    ...
    play(...) / play_coreaudio(...);
    if (stop_requested) break;
}
```

This ensures that after the inner function returns early due to Esc, the outer loop does not advance to the next file.

## What is not changing

- `--stdout` mode: no terminal raw mode, no Esc detection.
- Build system, headers, Makefile: unchanged.
- OSS `init_oss()` path: unchanged.
