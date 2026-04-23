/*  VTXplugin - VTX player for XMMS
 *
 *  Copyright (C) 2002-2003 Sashnov Alexander
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#if !defined(__APPLE__)
#include <sys/soundcard.h>
#endif

#include "ayemu.h"
#include "config.h"

static const int DEBUG = 0;

#define DEVICE_NAME "/dev/dsp"

struct option options[] = {
  { "quiet",   no_argument, NULL, 'q'},
  { "rand",    no_argument, NULL, 'Z'},
  { "taganrog",no_argument, NULL, 'T'},
  { "stdout",  no_argument, NULL, 's'},
  { "verbose", no_argument, NULL, 'v'},
  { "version", no_argument, NULL, 'V'},
  { "usage",   no_argument, NULL, 'u'},
  { "help",    no_argument, NULL, 'h'},
  { 0, 0, 0, 0}
};

char short_opts[] = "+qZTsvuh";

int qflag = 0;  // quite plating
int Zflag = 0;  // random list
int Tflag = 0;  // Taganrog, 3.5 MHz
int sflag = 0;  // to stdout
int vflag = 0;  // verbose

static volatile int stop_requested = 0;

static struct termios saved_termios;
static int term_is_raw = 0;

ayemu_ay_t ay;
ayemu_ay_reg_frame_t regs;

void *audio_buf = NULL;
int audio_bufsize;
int audio_fd = STDOUT_FILENO;

int  freq = 44100;
int  chans = 2;
int  bits = 16;

void usage ()
{
    printf(
	   "AY/YM VTX format player.\n"
	   "It uses libayemu (http://libayemu.sourceforge.net)\n"
	   "THIS SOFTWARE COMES WITH ABSOLUTELY NO WARRANTY! USE AT YOUR OWN RISK!\n\n"
	   "Usage: playvtx [option(s)] files...\n"
	   "  -q --quiet\tquiet (don't print title)\n"
	   "  -Z --random\tshuffle play\n"
       "  -T --taganrog\tForce use 3.5 MHz AY frequency as in Taganrog ZX clone\n"
	   "  -s --stdout\twrite sound data to stdout in .au format\n"
       "        You may then convert it to mp3 by:\n"
       "        playvtx --stdout secret.vtx > secret.raw\n"
       "        sox -r 44100 -e signed -b 16 -c 2 secret.raw secret.mp3\n"
	   "  -v --verbose\tincrease verbosity level\n"
	   "  --version\tshow programm version\n"
	   "  -h --help\n"
	   "  -u --usage\tthis help\n"
	   );
}

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

static void poll_esc(void)
{
  if (!term_is_raw) return;
  char c;
  if (read(STDIN_FILENO, &c, 1) == 1 && c == 0x1B)
    stop_requested = 1;
}

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

void play (const char *filename)
{
  ayemu_vtx_t *vtx;
  int len;

  vtx = ayemu_vtx_load_from_file(filename);
  if (!vtx) return;

  if (!qflag)
    printf(" Title: %s\n Author: %s\n From: %s\n Comment: %s\n Year: %d\n",
	   vtx->title, vtx->author, vtx->from, vtx->comment, vtx->year);

  int audio_bufsize = freq * chans * (bits >> 3) / vtx->playerFreq;
  if ((audio_buf = malloc (audio_bufsize)) == NULL) {
    fprintf (stderr, "Can't allocate sound buffer\n");
    goto free_vtx;
  }

  ayemu_reset(&ay);
  ayemu_set_chip_type(&ay, vtx->chiptype, NULL);
  ayemu_set_stereo(&ay, vtx->stereo, NULL);

  if (!Tflag)
    ayemu_set_chip_freq(&ay, vtx->chipFreq);
  else
    ayemu_set_chip_freq(&ay, 3500000); // in Taganrog AY freq got from the same pin as for Z80 CPU

  size_t pos;

  for (pos = 0; pos < vtx->frames; pos++) {
    ayemu_vtx_getframe (vtx, pos, regs);
    ayemu_set_regs (&ay, regs);
    ayemu_gen_sound (&ay, audio_buf, audio_bufsize);
    if ((len = write(audio_fd, audio_buf, audio_bufsize)) == -1) {
      fprintf (stderr, "Error writting to sound device, break.\n");
      break;
    }
  }

 free_vtx:
  ayemu_vtx_free(vtx);
}


int main (int argc, char **argv)
{
  int index;
  int c;
  int option_index = 0;

  opterr = 0;

  while ((c = getopt_long (argc, argv, short_opts, options, &option_index)) != -1)
    switch (c)
      {
      case 'q':
	qflag = 1;
	break;
      case 'Z':
	Zflag = 1;
	break;
      case 'T':
	Tflag = 1;
	break;
      case 's':
	sflag = 1;
	break;
      case 'v':
	vflag = 1;
	break;
      case 'V':
	printf ("playvtx %s\n", VERSION);
	exit (0);
      case 'h':
      case 'u':
	usage ();
	exit (0);
      case '?':
	if (isprint (optopt))
	  fprintf (stderr, "Unknown option `-%c'.\n", optopt);
	else
	  fprintf (stderr,
		   "Unknown option character `\\x%x'.\n",
		   optopt);
	usage ();
	return 1;
      case -1:
	break;
      default:
	abort ();
      }

  if (DEBUG)
    printf ("qflag = %d, Zflag = %d, sflag = %d, vflag = %d\n",
	    qflag, Zflag, sflag, vflag);

  if (Zflag)
    printf ("The -Z flag is not implemented yet, sorry\n");

  if (optind == argc) {
    fprintf (stderr, "No files to play specified, see %s --usage.\n",
	     argv[0]);
    exit (1);
  }

#if !defined(__APPLE__)
  if (! sflag)
    init_oss();  /* CoreAudio setup is per-file on macOS, no shared init */
#endif

  if (DEBUG)
    printf ("OSS sound system initialization success: bits=%d, chans=%d, freq=%d\n",
	    bits, chans, freq);

  ayemu_init(&ay);
  ayemu_set_sound_format(&ay, freq, chans, bits);

  for (index = optind; index < argc; index++) {
    printf ("\nPlaying file %s\n", argv[index]);
#ifdef __APPLE__
    if (!sflag) play_coreaudio(argv[index]);
    else        play(argv[index]);
#else
    play(argv[index]);
#endif
  }

  return 0;
}
