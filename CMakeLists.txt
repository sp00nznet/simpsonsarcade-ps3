cmake_minimum_required(VERSION 3.20)
project(simpsons_recomp
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "The Simpsons Arcade Game — static recompilation (NPUB30563)")

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- game data dir (override with -DSIMPSONS_GAME_DIR_OPT=...) ----
set(SIMPSONS_GAME_DIR_OPT "${CMAKE_CURRENT_SOURCE_DIR}/game" CACHE PATH
    "Path to game data directory")

# ---- ps3recomp SDK location (sibling ../../ps3 by default) ----
if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../ps3" ABSOLUTE)
endif()
message(STATUS "ps3recomp SDK: ${PS3RECOMP_DIR}")

# Prefer a prebuilt runtime lib; otherwise build it from source.
set(PS3RECOMP_RUNTIME_DIR "${PS3RECOMP_DIR}/build" CACHE PATH
    "Directory containing the prebuilt ps3recomp_runtime library")
set(_CANDS
    "${PS3RECOMP_RUNTIME_DIR}/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/Release/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/Debug/ps3recomp_runtime.lib"
    "${PS3RECOMP_RUNTIME_DIR}/libps3recomp_runtime.a")
set(_FOUND "")
foreach(_c ${_CANDS})
    if(EXISTS "${_c}")
        set(_FOUND "${_c}")
        break()
    endif()
endforeach()
if(_FOUND)
    message(STATUS "ps3recomp runtime: ${_FOUND}")
    add_library(ps3recomp_runtime STATIC IMPORTED)
    set_target_properties(ps3recomp_runtime PROPERTIES IMPORTED_LOCATION "${_FOUND}")
    target_include_directories(ps3recomp_runtime INTERFACE "${PS3RECOMP_DIR}/include")
else()
    message(STATUS "Building ps3recomp runtime from source")
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
endif()

# ---- generated recompiled code (src/recomp/, not in git) ----
file(GLOB_RECURSE RECOMP_SOURCES "src/recomp/*.c" "src/recomp/*.cpp")
if(NOT RECOMP_SOURCES)
    message(WARNING "No src/recomp/ sources — run the lifter + tools/gen_runtime.py first")
endif()

add_executable(simpsons
    src/main.cpp
    src/elf_loader.cpp
    src/vm_bridge.cpp
    src/hle_modules.cpp
    src/gcm_bridge.cpp
    src/spurs_hle.cpp
    src/indirect_dispatch.cpp
    src/malloc_override.cpp
    src/crash_handler.cpp
    src/spu_runner.cpp
    src/spu_runner_spurs.cpp
    src/spu/spu_channels.c
    src/spu/cri_job.c
    src/spu/spurs_kernel.c
    src/spu/spurs_policy.c
    ${RECOMP_SOURCES})

target_include_directories(simpsons PRIVATE
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp")

target_link_libraries(simpsons PRIVATE ps3recomp_runtime)

target_compile_definitions(simpsons PRIVATE
    SIMPSONS_GAME_DIR="${SIMPSONS_GAME_DIR_OPT}"
    SIMPSONS_TITLE_ID="NPUB30563")

if(WIN32)
    target_link_libraries(simpsons PRIVATE ws2_32 xinput ole32 bcrypt d3d12 dxgi uuid dxguid)
endif()

if(MSVC)
    target_compile_options(simpsons PRIVATE /W3 /wd4100 /wd4244 /wd4267 /wd4996 /wd4013 /Zc:__cplusplus /bigobj)
    target_link_options(simpsons PRIVATE /STACK:0x10000000)
    target_compile_definitions(simpsons PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(simpsons PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
endif()

install(TARGETS simpsons RUNTIME DESTINATION bin)
