# Host tools. Build with: make -C tools
CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CORE = ../src/core
BIN = ../build/host

TOOLS = $(BIN)/umdinfo $(BIN)/umddemux $(BIN)/at3pdec $(BIN)/h264prog $(BIN)/pngdec $(BIN)/playtest $(BIN)/imgcmp
AT3P = $(wildcard $(CORE)/atrac3p/*.c)
# The FFmpeg-derived files keep upstream's style; silence the warnings it triggers.
AT3P_CFLAGS = -Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-shift-op-parentheses -Wno-parentheses

all: $(TOOLS)

$(BIN)/umdinfo: umdinfo.c host_source.c $(CORE)/disc_image.c $(CORE)/png.c $(CORE)/iso9660.c $(CORE)/sfo.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/umddemux: umddemux.c host_source.c $(CORE)/disc_image.c $(CORE)/png.c $(CORE)/iso9660.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c $(CORE)/demux.c $(CORE)/h264.c $(CORE)/vtiming.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/at3pdec: at3pdec.c $(AT3P) $(wildcard $(CORE)/atrac3p/*.h) | $(BIN)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -o $@ at3pdec.c $(AT3P) -lm

$(BIN)/h264prog: h264prog.c $(CORE)/h264.c ../src/platform/macos/h264_progressive.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/imgcmp: imgcmp.c host_source.c $(CORE)/disc_image.c $(CORE)/png.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/pngdec: pngdec.c $(CORE)/png.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

PLAYER_CORE = $(CORE)/disc_image.c $(CORE)/iso9660.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c $(CORE)/demux.c $(CORE)/h264.c $(CORE)/vtiming.c \
              $(CORE)/png.c $(CORE)/canvas.c $(CORE)/font8x8.c $(CORE)/cropdetect.c $(CORE)/sfo.c $(CORE)/icons.c
$(BIN)/playtest: playtest.c ../src/app/main.c ../src/app/library.c ../src/app/player.c ../src/app/screen.c ../src/app/osd.c ../src/app/resume.c ../src/platform/null/null_platform.c \
                 $(PLAYER_CORE) $(AT3P) $(wildcard ../src/app/*.h ../src/core/*.h) | $(BIN)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -o $@ playtest.c ../src/app/main.c ../src/app/library.c ../src/app/player.c ../src/app/screen.c ../src/app/osd.c ../src/app/resume.c \
	    ../src/platform/null/null_platform.c $(PLAYER_CORE) $(AT3P) -lm -lpthread

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
