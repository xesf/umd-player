cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK})
  set(ENV{VITASDK} "$ENV{HOME}/vitasdk")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(at3p_bench C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "UMD audio benchmark")
set(VITA_TITLEID  "UMDP00091")
set(VITA_VERSION  "01.00")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -mcpu=cortex-a9 -mfpu=neon -Wall -Wextra")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
file(GLOB AT3P_SOURCES ${ROOT}/src/core/atrac3p/*.c)
# The FFmpeg-derived files keep upstream's style; silence the warnings it triggers.
set_source_files_properties(${AT3P_SOURCES} PROPERTIES
  COMPILE_FLAGS "-Wno-pointer-sign -Wno-unused-parameter -Wno-sign-compare -Wno-parentheses")

add_executable(at3p_bench
  main.c
  ../common/test_log.c
  ${AT3P_SOURCES}
  ${ROOT}/src/core/iso9660.c
  ${ROOT}/src/core/umdv.c
  ${ROOT}/src/core/mpeg_ps.c
  ${ROOT}/src/core/demux.c
  ${ROOT}/src/platform/vita/vita_source.c
)

target_link_libraries(at3p_bench
  m
  SceAudio_stub
  ScePower_stub
  SceDisplay_stub
  SceCtrl_stub
)

vita_create_self(at3p_bench.self at3p_bench)
vita_create_vpk(at3p_bench.vpk ${VITA_TITLEID} at3p_bench.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
