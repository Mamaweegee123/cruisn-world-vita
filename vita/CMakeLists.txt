cmake_minimum_required(VERSION 3.20)
project(CruisnWorldVita C CXX)

# Cross-compiled separately from runtime/CMakeLists.txt (the desktop project).
# This file must never be pulled into the desktop build; it only makes sense
# when configured with -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
# (see tools/scripts/build_vita.sh). Desktop keeps its own independent
# CMakeLists.txt/build tree untouched.
if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR
        "VITASDK is not set. Install VitaSDK and export VITASDK=/path/to/vitasdk "
        "before configuring this project -- see docs/research/VITA_BUILD.md.")
endif()

include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}/.." rev-parse --short HEAD
    OUTPUT_VARIABLE CRUISN_VITA_BUILD_GIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
execute_process(
    COMMAND git -C "${CMAKE_SOURCE_DIR}/.." status --porcelain
    OUTPUT_VARIABLE CRUISN_VITA_BUILD_STATUS
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CRUISN_VITA_BUILD_GIT)
    set(CRUISN_VITA_BUILD_GIT "unknown")
endif()
if(CRUISN_VITA_BUILD_STATUS)
    set(CRUISN_VITA_BUILD_DIRTY 1)
else()
    set(CRUISN_VITA_BUILD_DIRTY 0)
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Every Vita package gets the same shell presentation.  Keep this wrapper as
# the only VPK entry point for Cruis'n diagnostic targets as well as the real
# runtime: a future diagnostic VPK only needs to call
# cruisn_create_branded_vpk(...) to receive the icon, startup art, and
# LiveArea bundle automatically.
#
# vita_create_vpk keeps packaging flags in CMake variables, so reset them in
# this function's scope before each invocation.  That prevents one VPK's
# resources or SFO fields from leaking into another target.
function(cruisn_create_branded_vpk target titleid eboot)
    set(VITA_MKSFOEX_FLAGS "")
    set(VITA_PACK_VPK_FLAGS "")
    vita_create_vpk(${target} ${titleid} ${eboot}
        ${ARGN}
        FILE sce_sys/icon0.png sce_sys/icon0.png
        FILE sce_sys/pic0.png sce_sys/pic0.png
        FILE sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
        FILE sce_sys/livearea/contents/logo0.png sce_sys/livearea/contents/logo0.png
        FILE sce_sys/livearea/contents/logo1.png sce_sys/livearea/contents/logo1.png
        FILE sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
        FILE sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
    )
endfunction()

add_compile_options(
    -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard
    -fno-strict-aliasing
)

# --- Shared Cruis'n runtime (same source tree as desktop; not duplicated) ---
# See docs/research/VITA_PORT_STATUS.md for exactly how far this gets today.
# RECOMP_ENABLE_MODS=OFF: dynamic code mods need LiveRecomp/sljit (a JIT
# compiler needing POSIX executable-memory APIs) and dlopen (native-library
# mods), neither available on VitaSDK. Desktop's default stays ON, unchanged
# -- this is a build option on the shared librecomp/CMakeLists.txt, not a
# scattered set of __vita__ conditionals through the mod subsystem. With mods
# off, LiveRecomp/N64Recomp aren't even built for Vita.
set(RECOMP_ENABLE_MODS OFF CACHE BOOL "Enable dynamic code mod (LiveRecomp/JIT) support" FORCE)
set(FMT_OS OFF CACHE BOOL "Include OS-specific APIs." FORCE)
add_subdirectory(
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime"
    "${CMAKE_BINARY_DIR}/N64ModernRuntime"
)

# Clean pacing-correlation build: retain fatal/startup messages while gating
# historical high-frequency Vita diagnostics in the shared runtime.
target_compile_definitions(ultramodern PRIVATE CRUISN_VITA_CLEAN_PACING=1)
target_compile_definitions(librecomp PRIVATE CRUISN_VITA_CLEAN_PACING=1)

# Matches runtime/CMakeLists.txt's desktop setup: a few ultramodern/librecomp
# files call Cruis'n's own gfx_trace.h diagnostic header directly.
target_include_directories(ultramodern PRIVATE "${CMAKE_SOURCE_DIR}/../runtime/src")
target_include_directories(librecomp PRIVATE "${CMAKE_SOURCE_DIR}/../runtime/src")

# --- Generated Cruis'n game code (SAME source as desktop, not duplicated) ---
# This is the actual recompiled N64 game logic. It only needs recomp.h's
# macros (fixed for __vita__ above) and cache_stubs.h; it does not depend on
# librecomp's mod system, so it can be tested independently of that blocker.
set(CRUISN_RECOMP_DIR "${CMAKE_SOURCE_DIR}/../build/recomp/functions")
file(GLOB CRUISN_RECOMPILED_SOURCES CONFIGURE_DEPENDS "${CRUISN_RECOMP_DIR}/*.c")
if(NOT CRUISN_RECOMPILED_SOURCES)
    message(FATAL_ERROR "No generated functions found. Run tools/scripts/recompile.sh first.")
endif()
add_library(cruisn_recompiled_vita STATIC
    ${CRUISN_RECOMPILED_SOURCES}
    "${CMAKE_SOURCE_DIR}/../runtime/src/cache_stubs.c"
    "${CMAKE_SOURCE_DIR}/../runtime/src/entrypoint_bridge.c"
    "${CMAKE_SOURCE_DIR}/../runtime/src/section_registration.c"
)
target_include_directories(cruisn_recompiled_vita PRIVATE
    "${CRUISN_RECOMP_DIR}"
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/N64Recomp/include"
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/librecomp/include"
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/ultramodern/include"
)
target_compile_options(cruisn_recompiled_vita PRIVATE
    "-include${CMAKE_SOURCE_DIR}/../runtime/src/cache_stubs.h"
)

# --- Smoke-test app -------------------------------------------------------
# Deliberately does NOT link the shared Cruis'n runtime/game code yet. Its
# only job is to prove the toolchain, vitaGL init, sceCtrl polling, and the
# ux0:data ROM/log path all work end to end on a real VPK before any of that
# complexity is combined with the (much larger, still-porting) game runtime.
# See docs/research/VITA_PORT_STATUS.md for exactly what this does and does
# not prove.
add_executable(cruisn_vita_smoke
    src/smoke_main.cpp
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/thirdparty/xxHash/xxhash.c"
)
target_include_directories(cruisn_vita_smoke PRIVATE
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/thirdparty/xxHash"
)
target_link_libraries(cruisn_vita_smoke PRIVATE
    vitaGL
    mathneon
    vitashark
    SceShaccCgExt
    SceShaccCg_stub
    taihen_stub
    SceDisplay_stub
    SceGxm_stub
    SceCtrl_stub
    SceTouch_stub
    SceMotion_stub
    SceSysmodule_stub
    SceAppMgr_stub
    ScePower_stub
    SceCommonDialog_stub
    SceKernelDmacMgr_stub
    m
)

vita_create_self(cruisn_vita_smoke.self cruisn_vita_smoke UNSAFE)
cruisn_create_branded_vpk(CruisnWorldVitaSmoke.vpk CRWX00001 cruisn_vita_smoke.self
    VERSION 01.00
    NAME "Cruis'n World Xtreme (Vita smoke test)"
)

# --- Real runtime target ---------------------------------------------------
# Links the actual shared Cruis'n game code + librecomp (RECOMP_ENABLE_MODS=OFF)
# + ultramodern into a real executable that calls recomp::start(). See
# vita/src/runtime_main.cpp and docs/research/VITA_PORT_STATUS.md for exactly
# how far this gets (compiles/links is verified; runtime behavior is not --
# no physical Vita or Vita3K was available in this environment).
add_executable(cruisn_world_vita
    src/runtime_main.cpp
    src/vita_pacing.cpp
    "${CMAKE_SOURCE_DIR}/../runtime/generated/cruisn_gfx_ucode.cpp"
    "${CMAKE_SOURCE_DIR}/../runtime/generated/cruisn_audio_ucode.cpp"
    "${CMAKE_SOURCE_DIR}/../runtime/src/xtreme_special_options.cpp"
    "${CMAKE_SOURCE_DIR}/../runtime/src/gfx_trace.cpp"
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/thirdparty/xxHash/xxhash.c"
    renderer/cruisn_rdp_decoder.cpp
    renderer/cruisn_vitagl_backend.cpp
)
target_include_directories(cruisn_world_vita PRIVATE
    "${CRUISN_RECOMP_DIR}"
    "${CMAKE_SOURCE_DIR}/../runtime/src"
    "${CMAKE_SOURCE_DIR}/../runtime/vendor/N64ModernRuntime/thirdparty/xxHash"
)
target_link_libraries(cruisn_world_vita PRIVATE
    cruisn_recompiled_vita
    librecomp
    ultramodern
    "-Wl,--whole-archive" pthread "-Wl,--no-whole-archive"
    vitaGL
    mathneon
    vitashark
    SceShaccCgExt
    SceShaccCg_stub
    taihen_stub
    SceDisplay_stub
    SceGxm_stub
    SceCtrl_stub
    SceTouch_stub
    SceMotion_stub
    SceSysmodule_stub
    SceAppMgr_stub
    ScePower_stub
    SceCommonDialog_stub
    SceKernelDmacMgr_stub
    m
)
target_compile_options(cruisn_world_vita PRIVATE
    -fno-strict-aliasing
    -ffunction-sections
    -fdata-sections
)
target_compile_definitions(cruisn_world_vita PRIVATE
    CRUISN_VITA_CLEAN_PACING=1
    CRUISN_VITA_BUILD_GIT="${CRUISN_VITA_BUILD_GIT}"
    CRUISN_VITA_BUILD_DIRTY=${CRUISN_VITA_BUILD_DIRTY}
)
# The vitaGL/Sce* import table added by the renderer is large enough that
# vita-elf-create's default segment layout has no room left for SCE metadata
# ("Cannot allocate N bytes for SCE data ... segment 1 overlaps"). A larger
# page size gives it more slack -- same fix noted in DK64-Vita's own
# CMakeLists for the same underlying vita-elf-create limitation.
target_link_options(cruisn_world_vita PRIVATE
    -Wl,--gc-sections
    -Wl,-z,max-page-size=0x10000
)

vita_create_self(cruisn_world_vita.self cruisn_world_vita UNSAFE)
cruisn_create_branded_vpk(CruisnWorldXtreme.vpk CRWX00002 cruisn_world_vita.self
    VERSION 01.00
    NAME "Cruis'n World Xtreme"
)
