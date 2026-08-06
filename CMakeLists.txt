cmake_minimum_required(VERSION 3.14)

if (NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Please define VITASDK env var to point to your SDK path or set CMAKE_TOOLCHAIN_FILE.")
endif ()

set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Toolchain file")

project(btd5_vita C CXX ASM)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Android ARMv7 binaries use the soft-float procedure-call ABI.  VitaSDK's
# packaged OpenSLES and zlib archives are hard-float, so build these two
# dependencies locally with scripts/build_softfp_deps.py before configuring.
set(SOFTFP_DEPS_DIR "${CMAKE_SOURCE_DIR}/.deps-softfp" CACHE PATH
    "Directory containing soft-float dependency archives")
foreach(SOFTFP_ARCHIVE libOpenSLES.a libz.a)
  if(NOT EXISTS "${SOFTFP_DEPS_DIR}/${SOFTFP_ARCHIVE}")
    message(FATAL_ERROR
      "Missing ${SOFTFP_DEPS_DIR}/${SOFTFP_ARCHIVE}. "
      "Run python3 scripts/build_softfp_deps.py before configuring this project.")
  endif()
endforeach()
if(NOT EXISTS "${CMAKE_SOURCE_DIR}/.deps-src/minimp3/minimp3_ex.h")
  message(FATAL_ERROR
    "Missing .deps-src/minimp3/minimp3_ex.h. "
    "Run python3 scripts/build_softfp_deps.py before configuring this project.")
endif()

# Vita application configuration
set(VITA_APP_NAME "Bloons TD 5")
set(VITA_TITLEID "BTD500001")
set(VITA_VPKNAME "btd5-vita")
set(VITA_VERSION "01.00")
set(VITA_RELEASE_VERSION "1.00")

# Optional: used for "send" and "dump" targets
set(PSVITAIP "$ENV{PSVITAIP}" CACHE STRING "PS Vita IP address (for FTP access)")
set(PARSECOREPATH "$ENV{PARSECOREPATH}" CACHE STRING "vita-parse-core main.py")

# Game-specific definitions.  Migrate caches created from the SoLoader
# boilerplate before this project was tailored for BTD5.  CMake otherwise
# preserves those obsolete template values across every subsequent build.
if (DATA_PATH STREQUAL "ux0:data/gamename/")
  unset(DATA_PATH CACHE)
endif()
if (SO_PATH STREQUAL "ux0:data/gamename/main.so")
  unset(SO_PATH CACHE)
endif()
set(DATA_PATH "ux0:data/btd5/" CACHE STRING "Path to user-supplied game data (with trailing /)")
set(SO_PATH "${DATA_PATH}libnative.so" CACHE STRING "Path to the Android ARMv7 executable")
add_definitions(-DDATA_PATH="${DATA_PATH}" -DSO_PATH="${SO_PATH}")

option(BTD5_NATIVE_PHASE_PROFILER
       "Compile the BTD5 4.7 native phase profiler; it installs only when loader logging is enabled" ON)
option(BTD5_FRAME_DEBT_CLAMP
       "Cap severe BTD5 4.7 frame debt at 33.33 ms before the active update" ON)
option(BTD5_RELEASE_TELEMETRY
       "Compile periodic telemetry; runtime collection occurs only in the logging VPK" ON)
if (BTD5_RELEASE_TELEMETRY)
  add_definitions(-DBTD5_RELEASE_TELEMETRY)
endif()
if (BTD5_RELEASE_TELEMETRY OR BTD5_NATIVE_PHASE_PROFILER)
  add_definitions(-DBTD5_PERIODIC_TELEMETRY)
endif()
if (BTD5_NATIVE_PHASE_PROFILER)
  add_definitions(-DBTD5_NATIVE_PHASE_PROFILER)
endif()
if (BTD5_FRAME_DEBT_CLAMP)
  add_definitions(-DBTD5_FRAME_DEBT_CLAMP)
endif()
if (BTD5_NATIVE_PHASE_PROFILER OR BTD5_FRAME_DEBT_CLAMP)
  set(BTD5_NATIVE_PATCH_SOURCES source/engine_callsite.S)
  set_source_files_properties(source/engine_callsite.S PROPERTIES
    COMPILE_FLAGS "-mcpu=cortex-a9 -mthumb")
else()
  set(BTD5_NATIVE_PATCH_SOURCES)
endif()

option(USE_SCELIBC_IO "Use SceLibcBridge for IO and some other std functions" ON)
if (USE_SCELIBC_IO)
  add_definitions(-DUSE_SCELIBC_IO)
endif()

set(SHADER_FORMAT "GLSL" CACHE STRING "Preferred shader format (one of 'GLSL', 'CG', 'GXP')")
if (${SHADER_FORMAT} STREQUAL "GLSL")
  add_definitions(-DUSE_GLSL_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "CG")
  add_definitions(-DUSE_CG_SHADERS)
elseif (${SHADER_FORMAT} STREQUAL "GXP")
  add_definitions(-DUSE_GXP_SHADERS)
else()
  message(FATAL_ERROR "SHADER_FORMAT option can only be one of: 'GLSL', 'CG', 'GXP'")
endif()

set(valid_dump_formats GLSL CG)
# BTD5's VitaGL path reports zero bytes from vglGetShaderBinary on this target.
# Keep runtime compilation enabled; it takes only a few milliseconds and avoids
# generating unusable empty GXP cache entries.
option(DUMP_COMPILED_SHADERS "When using GLSL or CG shader format, cache compiled shaders on disk" OFF)
if (DUMP_COMPILED_SHADERS)
  if (SHADER_FORMAT IN_LIST valid_dump_formats)
    add_definitions(-DDUMP_COMPILED_SHADERS)
  else()
    message(WARNING "DUMP_COMPILED_SHADERS has no effect for the chosen shader format ${SHADER_FORMAT}")
  endif()
endif()

# Unique debug definition for loader's logging to not interfere with anything
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_definitions(-DDEBUG_SOLOADER)
  # Uncomment this for verbose FalsoJNI logging:
  #add_definitions(-DFALSOJNI_DEBUGLEVEL=0)
  # Uncomment this for OpenGL logging:
  #add_definitions(-DDEBUG_OPENGL)
endif()

# makes sincos, sincosf, etc. visible
add_definitions(-D_GNU_SOURCE -D__POSIX_VISIBLE=999999)
add_definitions(-DBTD5_RUNTIME_LOGGING)

# The loader and compatibility shims execute on Vita's Cortex-A9/NEON CPU.
# Keep Android's soft-float call ABI while allowing local bridge/audio/input
# code to use the actual processor and omit frame pointers in Release builds.
set(BTD5_CPU_FLAGS "-mcpu=cortex-a9 -mtune=cortex-a9 -mfpu=neon -mfloat-abi=softfp")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q ${BTD5_CPU_FLAGS} -std=gnu11 -Wno-deprecated")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wl,-q ${BTD5_CPU_FLAGS} -std=gnu++20 -Wno-write-strings -Wno-psabi")

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O1 -g3")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O1 -g3")
else ()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g0 -ffast-math -fomit-frame-pointer")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g0 -ffast-math -fomit-frame-pointer")
endif()

add_executable(${CMAKE_PROJECT_NAME}
               ${BTD5_NATIVE_PATCH_SOURCES}
               source/diagnostics.c
               source/game.c
               source/music.c
               source/main.c
               source/dynlib.c
               source/java.c
               source/patch.c
               source/patch.h
               source/reimpl/bits/_ctype.c
               source/reimpl/bionic.c
               source/reimpl/controls.c
               source/reimpl/controls.h
               source/reimpl/ui_context.c
               source/reimpl/ui_context.h
               source/reimpl/egl.c
               source/reimpl/errno.c
               source/reimpl/io.c
               source/reimpl/jmp.c
               source/reimpl/log.c
               source/reimpl/mem.c
               source/reimpl/pthr.c
               source/reimpl/sys.c
               source/reimpl/time64.c
               source/reimpl/window.c
               source/reimpl/sndfile_stub.c
               source/reimpl/vitagl_splash_stub.c
               source/reimpl/asset_manager.cpp
               source/utils/dialog.c
               source/utils/glutil.c
               source/utils/init.c
               source/utils/logger.c
               source/utils/settings.c
               source/utils/utils.c
               lib/falso_jni/FalsoJNI.c
               lib/falso_jni/FalsoJNI_ImplBridge.c
               lib/falso_jni/FalsoJNI_Logger.c
               lib/falso_jni/converter.c
               lib/sha1/sha1.c
               lib/fios/fios.c
               lib/so_util/so_util.c)

add_subdirectory(lib/libc_bridge)
add_dependencies(${CMAKE_PROJECT_NAME} SceLibcBridge)

target_include_directories(${CMAKE_PROJECT_NAME} SYSTEM
                           PUBLIC ${CMAKE_SOURCE_DIR}/lib
                           )

target_include_directories(${CMAKE_PROJECT_NAME}
                           PUBLIC ${CMAKE_SOURCE_DIR}/source
                                  ${CMAKE_SOURCE_DIR}/.deps-src/minimp3
                           )

target_link_libraries(${CMAKE_PROJECT_NAME}
                      -Wl,--whole-archive pthread -Wl,--no-whole-archive
                      ${SOFTFP_DEPS_DIR}/libOpenSLES.a
                      stdc++
                      vitaGL
                      vitashark
                      SceShaccCgExt
                      taihen_stub
                      SceShaccCg_stub
                      ${SOFTFP_DEPS_DIR}/libz.a
                      m
                      ScePgf_stub
                      mathneon
                      kubridge_stub
                      ${CMAKE_BINARY_DIR}/lib/libc_bridge/stubs/libSceLibcBridge_stub.a
                      SceHid_stub
                      SceTouch_stub
                      SceMotion_stub
                      SceAppMgr_stub
                      SceAppUtil_stub
                      SceAudio_stub
                      SceAudioIn_stub
                      SceCommonDialog_stub
                      SceDisplay_stub
                      SceKernelDmacMgr_stub
                      SceSysmem_stub
                      SceFios2_stub
                      SceCtrl_stub
                      SceGxm_stub
                      ScePower_stub
                      SceTouch_stub
                      SceVshBridge_stub
                      )

# VitaGL has a partial EGL implementation.  The SoLoader EGL bridge provides
# the Android behaviour BTD5 needs, so permit those intentional overrides.
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--allow-multiple-definition)

vita_create_self(eboot.bin ${CMAKE_PROJECT_NAME} UNSAFE NOASLR)

# Keep VPK creation explicit and VERBATIM so every asset path is safely quoted.
set(BTD5_SFO "${CMAKE_CURRENT_BINARY_DIR}/${VITA_VPKNAME}.sfo")
set(BTD5_VPK_NOLOG
    "${CMAKE_CURRENT_BINARY_DIR}/${VITA_VPKNAME}-v${VITA_RELEASE_VERSION}.vpk")
set(BTD5_VPK_LOGGING
    "${CMAKE_CURRENT_BINARY_DIR}/${VITA_VPKNAME}-v${VITA_RELEASE_VERSION}-logging.vpk")
add_custom_command(
    OUTPUT "${BTD5_SFO}"
    COMMAND "${VITASDK}/bin/vita-mksfoex"
            -d ATTRIBUTE2=12
            -s APP_VER=${VITA_VERSION}
            -s TITLE_ID=${VITA_TITLEID}
            "${VITA_APP_NAME}" "${BTD5_SFO}"
    DEPENDS eboot.bin
    VERBATIM)

function(add_btd5_vpk OUTPUT_PATH LOG_CONFIG)
  add_custom_command(
      OUTPUT "${OUTPUT_PATH}"
      COMMAND "${VITASDK}/bin/vita-pack-vpk"
              -s "${BTD5_SFO}"
              -b "${CMAKE_CURRENT_BINARY_DIR}/eboot.bin"
              -a "${CMAKE_SOURCE_DIR}/extras/cpuinfo=cpuinfo"
              -a "${CMAKE_SOURCE_DIR}/extras/meminfo=meminfo"
              -a "${CMAKE_SOURCE_DIR}/extras/urandom=urandom"
              -a "${CMAKE_SOURCE_DIR}/${LOG_CONFIG}=loader_logging.cfg"
              -a "${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png=sce_sys/icon0.png"
              -a "${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png=sce_sys/pic0.png"
              -a "${CMAKE_SOURCE_DIR}/extras/livearea/startup.png=sce_sys/livearea/contents/startup.png"
              -a "${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png=sce_sys/livearea/contents/bg0.png"
              -a "${CMAKE_SOURCE_DIR}/extras/livearea/template.xml=sce_sys/livearea/contents/template.xml"
              "${OUTPUT_PATH}"
      DEPENDS "${BTD5_SFO}" eboot.bin
              "${CMAKE_SOURCE_DIR}/extras/cpuinfo"
              "${CMAKE_SOURCE_DIR}/extras/meminfo"
              "${CMAKE_SOURCE_DIR}/extras/urandom"
              "${CMAKE_SOURCE_DIR}/${LOG_CONFIG}"
              "${CMAKE_SOURCE_DIR}/extras/livearea/icon0.png"
              "${CMAKE_SOURCE_DIR}/extras/livearea/pic0.png"
              "${CMAKE_SOURCE_DIR}/extras/livearea/startup.png"
              "${CMAKE_SOURCE_DIR}/extras/livearea/bg0.png"
              "${CMAKE_SOURCE_DIR}/extras/livearea/template.xml"
      VERBATIM)
endfunction()

add_btd5_vpk("${BTD5_VPK_NOLOG}" "extras/config/loader-log-off.cfg")
add_btd5_vpk("${BTD5_VPK_LOGGING}" "extras/config/loader-log-on.cfg")
add_custom_target(${VITA_VPKNAME}_vpk ALL
                  DEPENDS "${BTD5_VPK_NOLOG}" "${BTD5_VPK_LOGGING}")
add_dependencies(${VITA_VPKNAME}_vpk eboot.bin-self)

add_custom_target(send
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ${VITA_TITLEID} | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}_vpk
                  )

add_custom_target(send_kvdb
                  COMMAND echo destroy | nc ${PSVITAIP} 1338
                  COMMAND curl -T eboot.bin ftp://${PSVITAIP}:1337/ux0:/app/${VITA_TITLEID}/
                  COMMAND echo launch ux0:/app/${VITA_TITLEID}/eboot.bin | nc ${PSVITAIP} 1338
                  DEPENDS ${VITA_VPKNAME}_vpk
                  )

add_custom_target(dump
                  COMMAND python3 ${CMAKE_SOURCE_DIR}/extras/scripts/get_dump.py ${PSVITAIP}
                  COMMAND python3 ${PARSECOREPATH} coredump ${CMAKE_PROJECT_NAME}
                  )

add_custom_target(reboot
                  COMMAND echo reboot | nc ${PSVITAIP} 1338
                  )
