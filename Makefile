# The desktop player. Build with: make
#   macOS and Linux build for the machine they run on.
#   Windows, from macOS or Linux with mingw-w64: make PLATFORM=windows
#   Linux needs the X11 and ALSA headers (Debian and Ubuntu: libx11-dev libasound2-dev).
# Run it:
#   make run                 the player for this machine
#   make run-windows         the Windows player under Wine
#   make run-linux           the Linux player: natively on Linux; on macOS in Docker, on a virtual
#                            screen for RUN_SECONDS, saving build/linux-docker/snapshot.ppm (with XQuartz,
#                            LINUX_DISPLAY=host.docker.internal:0 opens a window instead)
#   ARGS=umd/movie.iso       what to open (a folder or a disc image); none: the default folder
# Mac app: make app builds build/macos/UMDPlayer.app; ARCHS="arm64 x86_64" makes it universal
# (in build/macos-universal) and needs macOS 11 or later.
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
       src/core/rco.c src/core/vag.c src/core/vsmx.c src/core/jpeg.c src/core/yuv.c src/core/hash.c \
       src/core/h264dec.c src/core/h264dec_slice.c src/core/h264dec_recon.c src/core/h264dec_deblock.c src/core/h264_cabac_tables.c
AT3P = $(wildcard src/core/atrac3p/*.c)
APP = src/app/main.c src/app/player.c src/app/screen.c src/app/osd.c src/app/library.c src/app/resume.c \
      src/app/backdrop.c src/app/backdrop_png.c src/app/manual.c src/app/manual_png.c src/app/settings.c src/app/intro.c src/app/disc.c src/app/menu.c src/app/sfx.c src/app/infosheet.c
SOFT = src/platform/soft/soft_video.c src/platform/soft/soft_compose.c

ifeq ($(PLATFORM),macos)
  CC ?= cc
  ifneq ($(ARCHS),)
    CC := $(CC) $(foreach a,$(ARCHS),-arch $(a)) -mmacosx-version-min=11.0
    BUILD = build/macos-universal
  endif
  PLAT = src/platform/posix/posix_sys.c src/platform/macos/mac_video.c src/platform/macos/mac_audio.c
  EXTRA_OBJS = $(BUILD)/src/platform/macos/mac_main.o $(BUILD)/src/platform/macos/mac_font.o
  LIBS = -framework Cocoa -framework QuartzCore -framework CoreVideo -framework AudioToolbox -framework CoreText
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
  PLAT = src/platform/windows/win_sys.c src/platform/windows/win_font.c src/platform/windows/win_main.c src/platform/windows/win_audio.c $(SOFT)
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

$(BUILD)/src/platform/macos/mac_font.o: src/platform/macos/mac_font.m src/platform/platform.h
	@mkdir -p $(dir $@)
	$(CC) -fobjc-arc -O2 -g -Wall -Wextra -c -o $@ $<

$(BUILD)/umdplayer_res.o: src/platform/windows/umdplayer.rc assets/app/umdplayer.ico
	@mkdir -p $(dir $@)
	$(WINDRES) -o $@ $<

VERSION := $(shell sed -n 's/.*UMDPLAYER_VERSION "\(.*\)".*/\1/p' src/app/version.h)
APP_BUNDLE = $(BUILD)/UMDPlayer.app

# The Mac app bundle, signed ad hoc (Apple silicon runs only signed code).
app: $(TARGET)
	rm -rf $(APP_BUNDLE) $(BUILD)/umdplayer.iconset
	mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources $(BUILD)/umdplayer.iconset
	cp $(TARGET) $(APP_BUNDLE)/Contents/MacOS/umdplayer
	sed 's/@VERSION@/$(VERSION)/' src/platform/macos/Info.plist > $(APP_BUNDLE)/Contents/Info.plist
	for s in 16 32 128; do \
	    sips -z $$s $$s assets/vita/icon0.png --out $(BUILD)/umdplayer.iconset/icon_$${s}x$${s}.png >/dev/null; done
	sips -z 64 64 assets/vita/icon0.png --out $(BUILD)/umdplayer.iconset/icon_32x32@2x.png >/dev/null
	cp $(BUILD)/umdplayer.iconset/icon_32x32.png $(BUILD)/umdplayer.iconset/icon_16x16@2x.png
	iconutil -c icns -o $(APP_BUNDLE)/Contents/Resources/umdplayer.icns $(BUILD)/umdplayer.iconset
	codesign --force --sign - $(APP_BUNDLE)

ARGS ?=
RUN_SECONDS ?= 15
LINUX_DISPLAY ?=
LINUX_IMAGE = ubuntu:22.04
LINUX_SETUP = apt-get update -qq >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq gcc make libx11-dev libasound2-dev xvfb >/dev/null

run: $(TARGET)
ifeq ($(PLATFORM),windows)
	wine $(TARGET) $(ARGS)
else
	./$(TARGET) $(ARGS)
endif

run-windows:
	$(MAKE) PLATFORM=windows
	wine build/windows/umdplayer.exe $(ARGS)

run-linux:
ifeq ($(UNAME),Linux)
	$(MAKE) PLATFORM=linux
	./build/linux/umdplayer $(ARGS)
else ifneq ($(LINUX_DISPLAY),)
	docker run --rm -it -e DISPLAY=$(LINUX_DISPLAY) -e HOME=/tmp -v "$(CURDIR)":/src -w /src $(LINUX_IMAGE) \
	    sh -c '$(LINUX_SETUP) && make PLATFORM=linux BUILD=build/linux-docker && ./build/linux-docker/umdplayer $(ARGS)'
else
	docker run --rm -v "$(CURDIR)":/src -w /src $(LINUX_IMAGE) sh -c '$(LINUX_SETUP) && make PLATFORM=linux BUILD=build/linux-docker && \
	    (Xvfb :1 -screen 0 1280x800x24 >/dev/null 2>&1 &) && sleep 2 && \
	    DISPLAY=:1 HOME=/tmp UMD_SNAPSHOT=build/linux-docker/snapshot.ppm UMD_SNAPSHOT_AFTER=$$(( $(RUN_SECONDS) * 700000 )) \
	    timeout $(RUN_SECONDS) ./build/linux-docker/umdplayer --window $(ARGS); true'
	@echo "picture: build/linux-docker/snapshot.ppm"
endif

clean:
	rm -rf $(BUILD)

.PHONY: all app clean run run-windows run-linux
