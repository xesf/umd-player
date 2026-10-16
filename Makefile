# The desktop player. Build with: make
#   macOS and Linux build for the machine they run on.
#   Windows, from macOS or Linux with mingw-w64: make PLATFORM=windows
#   Linux needs the X11 and ALSA headers (Debian and Ubuntu: libx11-dev libasound2-dev).
# Host tools: make -C tools. The Vita build uses CMake: see the README.
UNAME := $(shell uname -s 2>/dev/null)
ifeq ($(PLATFORM),)
  ifeq ($(OS),Windows_NT)
    PLATFORM = windows
  else ifeq ($(UNAME),Darwin)
    PLATFORM = macos
  else
    PLATFORM = linux
  endif
endif

CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
AT3P_CFLAGS = -Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-parentheses
BUILD = build/$(PLATFORM)

CORE = src/core/disc_image.c src/core/iso9660.c src/core/umdv.c src/core/mpeg_ps.c src/core/demux.c src/core/h264.c \
       src/core/vtiming.c src/core/deint.c src/core/png.c src/core/canvas.c src/core/font8x8.c src/core/cropdetect.c src/core/sfo.c src/core/icons.c \
       src/core/rco.c src/core/vsmx.c src/core/jpeg.c src/core/yuv.c \
       src/core/h264dec.c src/core/h264dec_slice.c src/core/h264dec_recon.c src/core/h264dec_deblock.c src/core/h264_cabac_tables.c
AT3P = $(wildcard src/core/atrac3p/*.c)
APP = src/app/main.c src/app/player.c src/app/screen.c src/app/osd.c src/app/library.c src/app/resume.c \
      src/app/backdrop.c src/app/backdrop_png.c src/app/settings.c src/app/intro.c src/app/disc.c src/app/menu.c
SOFT = src/platform/soft/soft_video.c src/platform/soft/soft_compose.c

ifeq ($(PLATFORM),macos)
  CC ?= cc
  PLAT = src/platform/posix/posix_sys.c src/platform/macos/mac_video.c src/platform/macos/mac_audio.c
  EXTRA_OBJS = $(BUILD)/src/platform/macos/mac_main.o
  LIBS = -framework Cocoa -framework QuartzCore -framework CoreVideo -framework AudioToolbox
  AT3P_CFLAGS += -Wno-shift-op-parentheses
  TARGET = $(BUILD)/umdplayer
else ifeq ($(PLATFORM),linux)
  AT3P_CFLAGS += -Wno-overflow
  PLAT = src/platform/posix/posix_sys.c src/platform/linux/linux_main.c src/platform/linux/linux_audio.c $(SOFT)
  LIBS = -lX11 -lasound -lpthread -lm
  TARGET = $(BUILD)/umdplayer
else ifeq ($(PLATFORM),windows)
  ifeq ($(OS),Windows_NT)
    CC = gcc
  else
    CC = x86_64-w64-mingw32-gcc
  endif
  WINDRES = $(patsubst %gcc,%windres,$(CC))
  PLAT = src/platform/windows/win_sys.c src/platform/windows/win_main.c src/platform/windows/win_audio.c $(SOFT)
  EXTRA_OBJS = $(BUILD)/umdplayer_res.o
  CFLAGS += -D__USE_MINGW_ANSI_STDIO=1
  AT3P_CFLAGS += -Wno-overflow
  LIBS = -mwindows -static -lwinmm -lgdi32 -lshell32
  TARGET = $(BUILD)/umdplayer.exe
endif

OBJS = $(patsubst %.c,$(BUILD)/%.o,$(CORE) $(APP) $(PLAT)) $(patsubst %.c,$(BUILD)/%.o,$(AT3P)) $(EXTRA_OBJS)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LIBS)

$(BUILD)/src/core/atrac3p/%.o: src/core/atrac3p/%.c $(wildcard src/core/atrac3p/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -c -o $@ $<

# Platform code uses system headers that are not strict C99.
$(BUILD)/src/platform/%.o: src/platform/%.c $(wildcard src/platform/*.h src/platform/*/*.h)
	@mkdir -p $(dir $@)
	$(CC) -std=gnu99 -O2 -g -Wall -Wextra -c -o $@ $<

$(BUILD)/%.o: %.c $(wildcard src/core/*.h src/app/*.h src/platform/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/src/platform/macos/mac_main.o: src/platform/macos/mac_main.m src/platform/platform.h src/platform/macos/mac_frame.h
	@mkdir -p $(dir $@)
	$(CC) -fobjc-arc -O2 -g -Wall -Wextra -c -o $@ $<

$(BUILD)/umdplayer_res.o: src/platform/windows/umdplayer.rc assets/app/umdplayer.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean
