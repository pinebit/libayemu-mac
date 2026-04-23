
AM_CPPFLAGS = -I$(top_srcdir)/include

bin_PROGRAMS        = playvtx
playvtx_SOURCES     = playvtx.c
playvtx_LDADD       = $(top_srcdir)/src/libayemu.la -lm

if HOST_DARWIN
playvtx_LDFLAGS = -framework CoreAudio -framework AudioToolbox
endif
