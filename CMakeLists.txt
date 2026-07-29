cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(FF6.elf C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
set(VITA_APP_NAME "Final Fantasy VI")
set(VITA_TITLEID  "FF6000000")
set(VITA_VERSION  "01.00")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -Wl,-q,--wrap,memcpy,--wrap,memmove,--wrap,memset -Wall -O3 -mfloat-abi=hard")
set(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS} -std=c++17")

option(FF6_DEBUG "Build with the black-screen-hunt instrumentation (GL logging, probes, watchdog, boot.log tracing)" OFF)
if(FF6_DEBUG)
  add_definitions(-DFF6_DEBUG)
endif()

set(VPK_FILE_ARGS
  ${CMAKE_SOURCE_DIR}/NotoSansJP-Regular.ttf NotoSansJP-Regular.ttf
  ${CMAKE_SOURCE_DIR}/NotoSansSC-Regular.ttf NotoSansSC-Regular.ttf
  ${CMAKE_SOURCE_DIR}/NotoSansKR-Regular.ttf NotoSansKR-Regular.ttf
)

# LiveArea artwork is optional so the project builds before it exists.
foreach(livearea_file
    sce_sys/icon0.png
    sce_sys/livearea/contents/bg0.png
    sce_sys/livearea/contents/startup.png
    sce_sys/livearea/contents/template.xml)
  if(EXISTS "${CMAKE_SOURCE_DIR}/${livearea_file}")
    list(APPEND VPK_FILE_ARGS
      ${CMAKE_SOURCE_DIR}/${livearea_file} ${livearea_file}
    )
  endif()
endforeach()

add_executable(FF6.elf
  loader/main.cpp
  loader/bridge.c
  loader/dialog.c
  loader/so_util.c
  loader/stb_image.c
  loader/stb_truetype.c
)

target_link_libraries(FF6.elf
  m
  stdc++
  vitaGL
  vitashark
  mathneon
  kubridge_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAudio_stub
  SceCtrl_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceIme_stub
  SceShaccCg_stub
  SceShaccCgExt
  SceSysmodule_stub
  ScePower_stub
  SceTouch_stub
  SceVshBridge_stub
  taihen_stub
  OpenSLES
  sndfile
  mpg123
  mp3lame
  opus
  opusfile
  FLAC
  vorbisenc
  vorbisfile
  vorbis
  ogg
  SDL
  SceAudio_stub
  SceHid_stub
  SceMotion_stub
  z
  -Wl,--whole-archive
  pthread
  -Wl,--no-whole-archive
)

vita_create_self(eboot.bin FF6.elf UNSAFE)

vita_create_vpk(FF6.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${VPK_FILE_ARGS}
)

add_custom_target(copy
  COMMAND echo "destroy" | nc $(PSVITAIP) 1338
  COMMAND curl -T eboot.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/eboot.bin DEPENDS eboot.bin
)

add_custom_target(launch
  COMMAND echo "screen on" | nc $(PSVITAIP) 1338
  COMMAND echo "destroy" | nc $(PSVITAIP) 1338
  COMMAND curl -T eboot.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/eboot.bin DEPENDS eboot.bin
  COMMAND echo "launch ${VITA_TITLEID}" | nc $(PSVITAIP) 1338
)
