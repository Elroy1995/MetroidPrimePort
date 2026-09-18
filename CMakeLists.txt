cmake_minimum_required(VERSION 3.25)
project(metroid_prime_port LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)

# Aurora provides the GameCube/Wii compatibility layer (MIT): GX, OS, PAD, DVD,
# CARD, VI, SI, THP, mtx, plus the SDL3/WebGPU application layer.
add_subdirectory(extern/aurora EXCLUDE_FROM_ALL)
if(TARGET aurora_mtx)
    target_compile_definitions(aurora_mtx PRIVATE MTX_USE_PS=1)
endif()

# MusyX is the game's audio engine; the vendored fork is portable.
add_subdirectory(extern/musyx EXCLUDE_FROM_ALL)

# Decompiled game + engine sources (see files.cmake).
include(files.cmake)
# Not portable: a raw PowerPC asm wrapper, and decomp-only type-matching scaffolding.
list(REMOVE_ITEM MP_GAME_SOURCES
    src/NESemu/modwrapper.cpp
    src/MetroidPrime/TypesMatch.cpp)
add_library(mp_game STATIC ${MP_GAME_SOURCES})
target_include_directories(mp_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/build/GM8E01_00/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include)
target_link_libraries(mp_game PUBLIC
    aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::thp aurora::card
    musyx)
# The decompiled code is written for MWCC; modern-compiler warnings are noise
# while we work through real errors. compat.h restores SDK macros Aurora omits.
target_compile_options(mp_game PRIVATE -w
    -Wno-narrowing -Wno-c++11-narrowing
    -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)

# Port entry point (Aurora owns the real main; see aurora/main.h).
add_executable(metroid_prime_port platform/main.cpp)
target_link_libraries(metroid_prime_port PRIVATE mp_game aurora::main)
