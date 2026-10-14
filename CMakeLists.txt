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
set(VITA_VERSION  "00.01")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra")

file(GLOB AT3P_SOURCES src/core/atrac3p/*.c)
# The FFmpeg-derived files keep upstream's style; silence the warnings it triggers.
set_source_files_properties(${AT3P_SOURCES} PROPERTIES
  COMPILE_FLAGS "-Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-parentheses")

add_executable(umdplayer
  src/app/main.c
  src/app/player.c
  src/core/iso9660.c
  src/core/umdv.c
  src/core/mpeg_ps.c
  src/core/demux.c
  src/core/h264.c
  src/core/vtiming.c
  src/core/deint.c
  ${AT3P_SOURCES}
  src/platform/vita/vita_main.c
  src/platform/vita/vita_sys.c
  src/platform/vita/vita_video.c
  src/platform/vita/vita_audio.c
  src/platform/vita/vita_source.c
)

target_link_libraries(umdplayer
  m
  SceVideodec_stub
  SceSysmodule_stub
  SceDisplay_stub
  SceCtrl_stub
  SceAudio_stub
  ScePower_stub
)

vita_create_self(umdplayer.self umdplayer)
vita_create_vpk(umdplayer.vpk ${VITA_TITLEID} umdplayer.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
