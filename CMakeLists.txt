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

# The version comes from src/app/version.h: "0.3.0" gives the package umdplayer-v0.3.0.vpk
# and the Vita app version 00.03.
file(STRINGS src/app/version.h UMDPLAYER_VERSION_LINE REGEX "#define UMDPLAYER_VERSION ")
string(REGEX MATCH "\"([0-9]+)\\.([0-9]+)\\.([0-9]+)\"" _ "${UMDPLAYER_VERSION_LINE}")
set(UMDPLAYER_VERSION "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/app/version.h)

set(VITA_APP_NAME "UMD Player")
set(VITA_TITLEID  "UMDP00001")
if(CMAKE_MATCH_1 LESS 10)
  set(VITA_MAJOR "0${CMAKE_MATCH_1}")
else()
  set(VITA_MAJOR "${CMAKE_MATCH_1}")
endif()
if(CMAKE_MATCH_2 LESS 10)
  set(VITA_MINOR "0${CMAKE_MATCH_2}")
else()
  set(VITA_MINOR "${CMAKE_MATCH_2}")
endif()
set(VITA_VERSION "${VITA_MAJOR}.${VITA_MINOR}")
set(VPK_NAME "umdplayer-v${UMDPLAYER_VERSION}.vpk")
message(STATUS "UMD Player ${UMDPLAYER_VERSION}: ${VPK_NAME}, Vita version ${VITA_VERSION}")

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
  src/app/intro.c
  src/app/disc.c
  src/app/menu.c
  src/core/rco.c
  src/core/vsmx.c
  src/core/jpeg.c
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
vita_create_vpk(${VPK_NAME} ${VITA_TITLEID} umdplayer.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE LICENSE LICENSE
  FILE NOTICE NOTICE
  FILE src/core/atrac3p/COPYING.LGPLv2.1 COPYING.LGPLv2.1
  FILE assets/vita/icon0.png sce_sys/icon0.png
  FILE assets/vita/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE assets/vita/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE assets/vita/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
)
