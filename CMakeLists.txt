cmake_minimum_required(VERSION 3.16)

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

option(FF6_DEBUG "Build with optional diagnostics (GL logging, frame timing, watchdog, boot.log and opt-in autoplay)" OFF)
if(FF6_DEBUG)
  add_definitions(-DFF6_DEBUG)
endif()

# These cache entries also allow a project-local build of the required fork,
# without replacing the vitaGL installed in the SDK used by other projects.
find_path(VITAGL_INCLUDE_DIR vitaGL.h
  HINTS "${VITASDK}/arm-vita-eabi/include")
find_library(VITAGL_LIBRARY NAMES vitaGL
  HINTS "${VITASDK}/arm-vita-eabi/lib")
if(NOT VITAGL_INCLUDE_DIR OR NOT VITAGL_LIBRARY)
  message(FATAL_ERROR "vitaGL was not found. Set VITAGL_INCLUDE_DIR and VITAGL_LIBRARY.")
endif()

include(CheckCSourceCompiles)
set(_ff6_required_includes "${CMAKE_REQUIRED_INCLUDES}")
set(CMAKE_REQUIRED_INCLUDES "${VITAGL_INCLUDE_DIR}")
set(_ff6_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(FF6_VITAGL_HAS_INPLACE_DECL CACHE)
check_c_source_compiles("#include <vitaGL.h>
int main(void) {
  void (*required)(GLboolean) = &vglInPlaceTexSubImage;
  return required == 0;
}" FF6_VITAGL_HAS_INPLACE_DECL)
if(_ff6_try_compile_target_type)
  set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_ff6_try_compile_target_type}")
else()
  unset(CMAKE_TRY_COMPILE_TARGET_TYPE)
endif()
set(CMAKE_REQUIRED_INCLUDES "${_ff6_required_includes}")
if(NOT FF6_VITAGL_HAS_INPLACE_DECL)
  message(FATAL_ERROR "This port needs the FF6 vitaGL patches: the selected header lacks vglInPlaceTexSubImage.")
endif()

# A declaration alone is not enough: mixing a fork header with an unpatched
# SDK archive otherwise fails late, after compiling the entire loader.
execute_process(COMMAND "${CMAKE_NM}" -g --defined-only "${VITAGL_LIBRARY}"
  RESULT_VARIABLE _ff6_vitagl_nm_result
  OUTPUT_VARIABLE _ff6_vitagl_symbols
  ERROR_VARIABLE _ff6_vitagl_nm_error)
if(NOT _ff6_vitagl_nm_result EQUAL 0 OR
   NOT _ff6_vitagl_symbols MATCHES "[ \t][TW][ \t]+vglInPlaceTexSubImage([\r\n]|$)")
  message(FATAL_ERROR "The selected vitaGL archive lacks vglInPlaceTexSubImage. Build the FF6 vitaGL patches and set VITAGL_LIBRARY. ${_ff6_vitagl_nm_error}")
endif()
if(FF6_DEBUG)
  foreach(_ff6_debug_symbol vgl_debug_log_draws vgl_debug_scene_splits)
    if(NOT _ff6_vitagl_symbols MATCHES "[ \t][BDR][ \t]+${_ff6_debug_symbol}([\r\n]|$)")
      message(FATAL_ERROR "FF6_DEBUG requires vitaGL symbol ${_ff6_debug_symbol}; use the documented instrumented dependency build.")
    endif()
  endforeach()
endif()

file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/build_profile.c"
  "#ifdef FF6_DEBUG\n"
  "const char ff6_build_profile[] __attribute__((used)) = \"FF6_BUILD_PROFILE=debug\";\n"
  "#else\n"
  "const char ff6_build_profile[] __attribute__((used)) = \"FF6_BUILD_PROFILE=release\";\n"
  "#endif\n")

set(VPK_FILE_ARGS
  ${CMAKE_SOURCE_DIR}/NotoSansJP-Regular.ttf NotoSansJP-Regular.ttf
  ${CMAKE_SOURCE_DIR}/NotoSansSC-Regular.ttf NotoSansSC-Regular.ttf
  ${CMAKE_SOURCE_DIR}/NotoSansKR-Regular.ttf NotoSansKR-Regular.ttf
  ${CMAKE_SOURCE_DIR}/LICENSE LICENSE
  ${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md THIRD_PARTY_NOTICES.md
  ${CMAKE_SOURCE_DIR}/licenses/OFL-Noto.txt licenses/OFL-Noto.txt
  ${CMAKE_SOURCE_DIR}/licenses/MIT-ImGui.txt licenses/MIT-ImGui.txt
  ${CMAKE_BINARY_DIR}/companion.bin companion.bin
)

file(GLOB FF6_POSTFX_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/shaders/*.cg")
foreach(shader ${FF6_POSTFX_SOURCES})
  get_filename_component(shader_name "${shader}" NAME)
  list(APPEND VPK_FILE_ARGS "${shader}" "shaders/${shader_name}")
endforeach()

# LiveArea artwork is optional so the project builds before it exists.
foreach(livearea_file
    sce_sys/icon0.png
    sce_sys/livearea/contents/bg0.png
    sce_sys/livearea/contents/startup.png
    sce_sys/livearea/contents/config.png
    sce_sys/livearea/contents/template.xml)
  if(EXISTS "${CMAKE_SOURCE_DIR}/${livearea_file}")
    list(APPEND VPK_FILE_ARGS
      ${CMAKE_SOURCE_DIR}/${livearea_file} ${livearea_file}
    )
  endif()
endforeach()

add_executable(FF6.elf
  ${CMAKE_CURRENT_BINARY_DIR}/build_profile.c
  loader/main.cpp
  loader/bridge.c
  loader/dialog.c
  loader/so_util.c
  loader/stb_truetype.c
)

target_include_directories(FF6.elf BEFORE PRIVATE "${VITAGL_INCLUDE_DIR}")

target_link_libraries(FF6.elf
  m
  stdc++
  "${VITAGL_LIBRARY}"
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

add_executable(companion.elf ${CMAKE_CURRENT_BINARY_DIR}/build_profile.c companion/main.cpp)
target_include_directories(companion.elf BEFORE PRIVATE "${VITAGL_INCLUDE_DIR}")
target_link_libraries(companion.elf
  m stdc++ imgui "${VITAGL_LIBRARY}" vitashark mathneon
  SceAppMgr_stub SceAppUtil_stub SceAudio_stub SceCtrl_stub SceCommonDialog_stub
  SceDisplay_stub SceKernelDmacMgr_stub SceGxm_stub SceShaccCg_stub SceShaccCgExt
  SceSysmodule_stub SceTouch_stub SceVshBridge_stub taihen_stub)
vita_create_self(companion.bin companion.elf UNSAFE)

vita_create_vpk(FF6.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${VPK_FILE_ARGS}
)
add_dependencies(FF6.vpk-vpk companion.bin-self)

add_custom_target(copy
  COMMAND echo "destroy" | nc $(PSVITAIP) 1338
  COMMAND curl -T eboot.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/eboot.bin DEPENDS eboot.bin
  COMMAND curl -T companion.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/companion.bin DEPENDS companion.bin
)

add_custom_target(launch
  COMMAND echo "screen on" | nc $(PSVITAIP) 1338
  COMMAND echo "destroy" | nc $(PSVITAIP) 1338
  COMMAND curl -T eboot.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/eboot.bin DEPENDS eboot.bin
  COMMAND curl -T companion.bin ftp://$(PSVITAIP):1337/ux0:/app/${VITA_TITLEID}/companion.bin DEPENDS companion.bin
  COMMAND echo "launch ${VITA_TITLEID}" | nc $(PSVITAIP) 1338
)
