# PS Vita build of the player. The macOS build uses the Makefile.
#   export VITASDK=~/vitasdk PATH=~/vitasdk/bin:$PATH
#   cmake -S . -B build/vita && cmake --build build/vita
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK})
  set(ENV{VITASDK} "$ENV{HOME}/vitasdk")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(umdplayer C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "UMD Player")
set(VITA_TITLEID  "UMDP00001")
set(VITA_VERSION  "00.03")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra")

file(GLOB AT3P_SOURCES src/core/atrac3p/*.c)
# The FFmpeg-derived files keep upstream's style; silence the warnings it triggers.
set_source_files_properties(${AT3P_SOURCES} PROPERTIES
  COMPILE_FLAGS "-Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-parentheses")

add_executable(umdplayer
  src/app/main.c
  src/app/player.c
  src/app/screen.c
  src/app/osd.c
  src/app/library.c
  src/app/backdrop.c
  src/app/settings.c
  src/app/backdrop_png.c
  src/app/resume.c
  src/core/disc_image.c
  src/core/iso9660.c
  src/core/umdv.c
  src/core/mpeg_ps.c
  src/core/demux.c
  src/core/h264.c
  src/core/vtiming.c
  src/core/deint.c
  src/core/png.c
  src/core/canvas.c
  src/core/font8x8.c
  src/core/cropdetect.c
  src/core/sfo.c
  src/core/icons.c
  ${AT3P_SOURCES}
  src/platform/vita/vita_main.c
  src/platform/vita/vita_sys.c
  src/platform/vita/vita_video.c
  src/platform/vita/vita_gfx.c
  src/platform/vita/vita_shaders.c
  src/platform/vita/vita_audio.c
  src/platform/vita/vita_source.c
)

target_link_libraries(umdplayer
  m
  SceVideodec_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceSysmodule_stub
  SceDisplay_stub
  SceCtrl_stub
  SceTouch_stub
  SceAudio_stub
  ScePower_stub
  SceRtc_stub
  SceAppUtil_stub
)

# UNSAFE: safe homebrew cannot open ux0:pspemu, where Adrenaline users keep their ISOs.
vita_create_self(umdplayer.self umdplayer UNSAFE)
# Bubble icon and LiveArea, drawn by tools/make_assets.py.
vita_create_vpk(umdplayer.vpk ${VITA_TITLEID} umdplayer.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE assets/vita/icon0.png sce_sys/icon0.png
  FILE assets/vita/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE assets/vita/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE assets/vita/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
