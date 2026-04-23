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
