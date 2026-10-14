cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{VITASDK})
  set(ENV{VITASDK} "$ENV{HOME}/vitasdk")
endif()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
endif()

project(avcdec_test C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "UMD decoder test")
set(VITA_TITLEID  "UMDP00090")
set(VITA_VERSION  "01.00")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_executable(avcdec_test
  main.c
  ../common/test_log.c
  ${ROOT}/src/core/iso9660.c
  ${ROOT}/src/core/umdv.c
  ${ROOT}/src/core/mpeg_ps.c
  ${ROOT}/src/core/demux.c
  ${ROOT}/src/platform/vita/vita_source.c
)

target_link_libraries(avcdec_test
  SceVideodec_stub
  SceSysmodule_stub
  SceDisplay_stub
  SceCtrl_stub
)

vita_create_self(avcdec_test.self avcdec_test)
vita_create_vpk(avcdec_test.vpk ${VITA_TITLEID} avcdec_test.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
)
