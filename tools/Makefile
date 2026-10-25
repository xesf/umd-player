# Host tools. Build with: make -C tools
CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CORE = ../src/core
BIN = ../build/host

TOOLS = $(BIN)/umdinfo $(BIN)/umddemux $(BIN)/at3pdec $(BIN)/pngdec $(BIN)/playtest $(BIN)/imgcmp $(BIN)/menutest $(BIN)/h264dectest
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

MENU_SRC = menutest.c host_source.c ../src/app/menu.c $(CORE)/rco.c $(CORE)/vag.c $(CORE)/vsmx.c $(CORE)/png.c $(CORE)/canvas.c \
           $(CORE)/font8x8.c $(CORE)/iso9660.c $(CORE)/umdv.c $(CORE)/disc_image.c $(CORE)/jpeg.c
$(BIN)/menutest: $(MENU_SRC) ../src/app/menu.h $(CORE)/rco.h $(CORE)/vsmx.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(MENU_SRC) -lm

H264DEC = $(CORE)/h264dec.c $(CORE)/h264dec_slice.c $(CORE)/h264dec_recon.c $(CORE)/h264dec_deblock.c $(CORE)/h264_cabac_tables.c
$(BIN)/h264dectest: h264dectest.c $(H264DEC) $(CORE)/h264dec.h $(CORE)/h264dec_int.h $(CORE)/h264_cabac_tables.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ h264dectest.c $(H264DEC)

$(BIN)/imgcmp: imgcmp.c host_source.c $(CORE)/disc_image.c $(CORE)/png.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^

$(BIN)/pngdec: pngdec.c $(CORE)/png.c $(CORE)/jpeg.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ $^ -lm

PLAYER_CORE = $(CORE)/disc_image.c $(CORE)/iso9660.c $(CORE)/umdv.c $(CORE)/mpeg_ps.c $(CORE)/demux.c $(CORE)/h264.c $(CORE)/vtiming.c \
              $(CORE)/png.c $(CORE)/canvas.c $(CORE)/font8x8.c $(CORE)/cropdetect.c $(CORE)/sfo.c $(CORE)/icons.c \
              $(CORE)/rco.c $(CORE)/vag.c $(CORE)/vsmx.c $(CORE)/jpeg.c $(CORE)/yuv.c $(CORE)/hash.c $(H264DEC)
$(BIN)/playtest: playtest.c ../src/app/main.c ../src/app/library.c ../src/app/backdrop.c ../src/app/backdrop_png.c ../src/app/manual.c ../src/app/manual_png.c ../src/app/settings.c ../src/app/intro.c ../src/app/disc.c ../src/app/menu.c ../src/app/sfx.c ../src/app/player.c ../src/app/screen.c ../src/app/osd.c ../src/app/resume.c ../src/app/infosheet.c ../src/platform/null/null_platform.c \
                 $(PLAYER_CORE) $(AT3P) $(wildcard ../src/app/*.h ../src/core/*.h) | $(BIN)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -o $@ playtest.c ../src/app/main.c ../src/app/library.c ../src/app/backdrop.c ../src/app/backdrop_png.c ../src/app/manual.c ../src/app/manual_png.c ../src/app/settings.c ../src/app/intro.c ../src/app/disc.c ../src/app/menu.c ../src/app/sfx.c ../src/app/player.c ../src/app/screen.c ../src/app/osd.c ../src/app/resume.c ../src/app/infosheet.c \
	    ../src/platform/null/null_platform.c $(PLAYER_CORE) $(AT3P) -lm -lpthread

$(BIN):
	mkdir -p $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
