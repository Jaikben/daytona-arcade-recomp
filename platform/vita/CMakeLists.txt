cmake_minimum_required(VERSION 3.20)
# A separate cross-build: never try to execute the ROM importers on the Vita.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(NOT DEFINED ENV{VITASDK})
        message(FATAL_ERROR "Set VITASDK to your VitaSDK installation first")
    endif()
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE FILEPATH "VitaSDK toolchain")
endif()
project(daytona_vita LANGUAGES C CXX)
if(NOT VITA)
    message(FATAL_ERROR "This directory requires the VitaSDK toolchain, not a desktop compiler")
endif()
if(NOT VITASDK)
    set(VITASDK "$ENV{VITASDK}")
endif()
include("${VITASDK}/share/vita.cmake")
get_filename_component(DAYTONA_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -fno-fast-math -ffp-contract=off -ffunction-sections -fdata-sections)

option(DAYTONA_VITA_COMPILE_CHECK "Compile runtime and frontend without ROM-derived code; produces no VPK" OFF)
option(DAYTONA_VITA_GPU_FAST "Experimental vita2d/GXM 3D path" OFF)
option(DAYTONA_VITA_DIAGNOSTICS "GXM startup/performance file logging (faults always recorded)" OFF)
# Keep the switch frontend-only; audio/render/game code is identical in both builds.
set_source_files_properties(main_gpu.cpp PROPERTIES COMPILE_DEFINITIONS
    "DAYTONA_VITA_DIAGNOSTICS=$<BOOL:${DAYTONA_VITA_DIAGNOSTICS}>")
set(DAYTONA_GEN_ROOT "${DAYTONA_ROOT}/build/gen" CACHE PATH "Host-generated i960, TGP and 68000 C++ root")
set(DAYTONA_VITA_TITLEID "DAYT00093" CACHE STRING "Nine-character Vita title ID")
if(NOT DAYTONA_VITA_COMPILE_CHECK)
    foreach(path daytona93/gen_table.cpp daytona93_tgp/tgp_gen.cpp daytona93_snd/snd_gen.cpp)
        if(NOT EXISTS "${DAYTONA_GEN_ROOT}/${path}")
            message(FATAL_ERROR "Missing ${DAYTONA_GEN_ROOT}/${path}. Run scripts/recompile.py with the HOST compiler first; no game code ships in the repository.")
        endif()
    endforeach()
endif()

set(SOFTFLOAT_DIR "${DAYTONA_ROOT}/extern/softfloat")
set(YMFM_DIR "${DAYTONA_ROOT}/extern/ymfm/src")
foreach(path "${SOFTFLOAT_DIR}/source/softfloat_state.c" "${YMFM_DIR}/ymfm_opn.cpp")
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR "Missing dependency ${path}. Run python3 scripts/setup.py --no-build on the host first.")
    endif()
endforeach()
include("${DAYTONA_ROOT}/cmake/softfloat_sources.cmake")
add_library(vita_softfloat STATIC ${SOFTFLOAT_SOURCES})
target_include_directories(vita_softfloat PUBLIC "${SOFTFLOAT_DIR}/source/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/softfloat" "${SOFTFLOAT_DIR}/source/8086-SSE")
target_compile_definitions(vita_softfloat PUBLIC SOFTFLOAT_FAST_INT64 LITTLEENDIAN=1 THREAD_LOCAL=
    PRIVATE SOFTFLOAT_ROUND_ODD INLINE_LEVEL=5 SOFTFLOAT_FAST_DIV32TO16 SOFTFLOAT_FAST_DIV64TO32)
# Only the main thread executes the board. The SDL callback consumes already
# converted samples and never calls SoftFloat. No Vita TLS runtime is required.
target_compile_options(vita_softfloat PRIVATE -w)

add_library(vita_i960 STATIC
    "${DAYTONA_ROOT}/src/i960/isa.cpp" "${DAYTONA_ROOT}/src/i960/decode.cpp"
    "${DAYTONA_ROOT}/src/i960/reach.cpp" "${DAYTONA_ROOT}/src/i960/fp.cpp")
target_include_directories(vita_i960 PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(vita_i960 PUBLIC vita_softfloat)
add_library(vita_trace STATIC "${DAYTONA_ROOT}/src/trace/trace.cpp")
target_include_directories(vita_trace PUBLIC "${DAYTONA_ROOT}/src")
add_library(vita_ymfm STATIC "${YMFM_DIR}/ymfm_opn.cpp" "${YMFM_DIR}/ymfm_adpcm.cpp" "${YMFM_DIR}/ymfm_ssg.cpp")
target_include_directories(vita_ymfm SYSTEM PUBLIC "${YMFM_DIR}")
target_compile_options(vita_ymfm PRIVATE -w)

# The same native runtime as the desktop; no refcore or interpreter is linked.
set(RUNTIME_FILES enhance cpu tgp m2_tgp_board geo raster video m2_board game_loop zip rom_import
    archive m2_replay_bus lockstep snd_cpu multipcm sound_board native_sample_mixer native_sound_sequencer native_sound_engine
    comm_board)
set(RUNTIME_SOURCES)
foreach(name IN LISTS RUNTIME_FILES)
    list(APPEND RUNTIME_SOURCES "${DAYTONA_ROOT}/src/runtime/${name}.cpp")
endforeach()
add_library(vita_runtime STATIC ${RUNTIME_SOURCES})
target_include_directories(vita_runtime PUBLIC "${DAYTONA_ROOT}/src")
target_link_libraries(vita_runtime PUBLIC vita_trace vita_i960 vita_ymfm)
target_compile_definitions(vita_runtime PUBLIC M2_ROMSET="daytona93") # the set the Vita build is made from
option(DAYTONA_VITA_RENDER_OPT "Exact tile/layer caching and optimized CPU rasterizer" ON)
if(DAYTONA_VITA_RENDER_OPT)
    # Public because the flag affects Video/Raster class layouts.
    target_compile_definitions(vita_runtime PUBLIC M2_VITA_RENDER_OPT=1)
endif()
# ZIP uses the existing built-in decoder. 7z is deliberately excluded to avoid
# an unbounded solid-archive working set on the handheld. Convert on the host.

# Require the target SDK package. Do not accidentally pick up host SDL2.
find_package(SDL2 2.0.22 CONFIG REQUIRED PATHS "${VITASDK}/arm-vita-eabi/lib/cmake/SDL2" NO_DEFAULT_PATH)
if(TARGET SDL2::SDL2-static)
    set(VITA_SDL_TARGET SDL2::SDL2-static)
else()
    set(VITA_SDL_TARGET SDL2::SDL2)
endif()
if(DAYTONA_VITA_GPU_FAST)
    include("${CMAKE_CURRENT_SOURCE_DIR}/pinned_vita2d.cmake")
    set(VITA_BUFFER_LICENSE FILE "${buffered_vita2d_source_SOURCE_DIR}/LICENSE" "licenses/libvita2d.txt")
endif()

if(DAYTONA_VITA_COMPILE_CHECK)
    if(DAYTONA_VITA_GPU_FAST)
        add_library(daytona_vita_frontend OBJECT main_gpu.cpp gpu_fast.cpp)
        target_include_directories(daytona_vita_frontend PRIVATE "${VITA2D_INCLUDE_DIR}")
    else()
        add_library(daytona_vita_frontend OBJECT main.cpp)
    endif()
    target_link_libraries(daytona_vita_frontend PRIVATE vita_runtime ${VITA_SDL_TARGET})
    message(STATUS "ROM-free COMPILE CHECK only: no linked game or installable VPK will be produced")
else()
    file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${DAYTONA_GEN_ROOT}/daytona93/*.cpp")
    list(APPEND GEN_SOURCES "${DAYTONA_GEN_ROOT}/daytona93_tgp/tgp_gen.cpp" "${DAYTONA_GEN_ROOT}/daytona93_snd/snd_gen.cpp")
    if(DAYTONA_VITA_GPU_FAST)
        add_executable(daytona_vita main_gpu.cpp gpu_fast.cpp ${GEN_SOURCES})
        target_include_directories(daytona_vita PRIVATE "${VITA2D_INCLUDE_DIR}")
        target_compile_definitions(daytona_vita PRIVATE DAYTONA_VITA_GPU_FAST=1)
        target_link_libraries(daytona_vita PRIVATE vita_runtime ${VITA_SDL_TARGET} ${VITA2D_LIBRARY}
            SceDisplay_stub SceGxm_stub SceSysmodule_stub SceCtrl_stub ScePgf_stub ScePvf_stub
            SceCommonDialog_stub SceAppMgr_stub SceIofilemgr_stub SceProcessmgr_stub ScePower_stub
            freetype png jpeg z pthread m)
        set(VITA_APP_NAME "Daytona Recomp GPU25")
    else()
        add_executable(daytona_vita main.cpp ${GEN_SOURCES})
        target_link_libraries(daytona_vita PRIVATE vita_runtime ${VITA_SDL_TARGET} SceCtrl_stub SceIofilemgr_stub SceProcessmgr_stub ScePower_stub pthread m)
        set(VITA_APP_NAME "Daytona Recomp")
    endif()
    target_link_options(daytona_vita PRIVATE -Wl,--gc-sections)
    set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-Wno-unused-label")
    vita_create_self(eboot.bin daytona_vita UNSAFE)
    vita_create_vpk(daytona_vita.vpk ${DAYTONA_VITA_TITLEID} eboot.bin
        VERSION "01.23" NAME "${VITA_APP_NAME}"
        FILE "${DAYTONA_ROOT}/THIRD_PARTY.md" "licenses/THIRD_PARTY.md"
        FILE "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY.md" "licenses/VITA.md"
        ${VITA_BUFFER_LICENSE})
endif()
