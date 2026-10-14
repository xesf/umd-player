# Host tools. Build with: make -C tools
CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
CORE = ../src/core
BIN = ../build/host

TOOLS = $(BIN)/umdinfo $(BIN)/umddemux $(BIN)/at3pdec $(BIN)/h264prog $(BIN)/pngdec
AT3P = $(wildcard $(CORE)/atrac3p/*.c)
# The FFmpeg-derived files keep upstream's style; silence the warnings it triggers.
AT3P_CFLAGS = -Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-shift-op-parentheses -Wno-parentheses

all: $(TOOLS)

$(BIN)/umdinfo: umdinfo.c host_source.c $(CORE)/iso9660.c $(CORE)/sfo.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/umddemux: umddemux.c host_source.c $(CORE)/iso9660.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c $(CORE)/demux.c $(CORE)/h264.c $(CORE)/vtiming.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/at3pdec: at3pdec.c $(AT3P) $(wildcard $(CORE)/atrac3p/*.h) | $(BIN)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -o $@ at3pdec.c $(AT3P) -lm

$(BIN)/h264prog: h264prog.c $(CORE)/h264.c ../src/platform/macos/h264_progressive.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/pngdec: pngdec.c $(CORE)/png.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
