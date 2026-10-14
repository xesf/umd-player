# macOS player. Build with: make
# Host tools: make -C tools. Vita builds: see tests/vita/*/README.md.
CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic
OBJCFLAGS = -fobjc-arc -O2 -g -Wall -Wextra
AT3P_CFLAGS = -Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-shift-op-parentheses -Wno-parentheses
FRAMEWORKS = -framework Cocoa -framework QuartzCore -framework VideoToolbox -framework CoreMedia \
             -framework CoreVideo -framework AudioToolbox
BUILD = build/macos

CORE = src/core/iso9660.c src/core/umdv.c src/core/mpeg_ps.c src/core/demux.c src/core/h264.c \
       src/core/vtiming.c src/core/deint.c src/core/png.c src/core/canvas.c src/core/font8x8.c src/core/cropdetect.c src/core/sfo.c src/core/icons.c
AT3P = $(wildcard src/core/atrac3p/*.c)
APP = src/app/main.c src/app/player.c src/app/screen.c src/app/osd.c src/app/library.c
MAC = src/platform/macos/mac_sys.c src/platform/macos/mac_video.c src/platform/macos/mac_audio.c \
      src/platform/macos/h264_progressive.c

OBJS = $(patsubst %.c,$(BUILD)/%.o,$(CORE) $(APP) $(MAC)) $(patsubst %.c,$(BUILD)/%.o,$(AT3P)) \
       $(BUILD)/src/platform/macos/mac_main.o

all: $(BUILD)/umdplayer

$(BUILD)/umdplayer: $(OBJS)
	$(CC) -o $@ $^ $(FRAMEWORKS)

$(BUILD)/src/core/atrac3p/%.o: src/core/atrac3p/%.c $(wildcard src/core/atrac3p/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(AT3P_CFLAGS) -c -o $@ $<

$(BUILD)/src/platform/macos/%.o: src/platform/macos/%.c $(wildcard src/platform/*.h src/platform/macos/*.h)
	@mkdir -p $(dir $@)
	$(CC) -std=c99 -O2 -g -Wall -Wextra -c -o $@ $<

$(BUILD)/%.o: %.c $(wildcard src/core/*.h src/app/*.h src/platform/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/src/platform/macos/mac_main.o: src/platform/macos/mac_main.m src/platform/platform.h src/platform/macos/mac_frame.h
	@mkdir -p $(dir $@)
	$(CC) $(OBJCFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all clean
