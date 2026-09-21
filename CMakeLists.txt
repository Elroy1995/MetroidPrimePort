cmake_minimum_required(VERSION 3.25)
project(metroid_prime_port LANGUAGES C CXX)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_COLOR_DIAGNOSTICS ON)
include(CTest)
option(MP_BUILD_TESTS "Build native-port regression tests" ON)
option(MP_ENABLE_SMOKE_DRIVER "Enable the opt-in real-disc lifecycle test driver" OFF)
option(MP_ANDROID_NOD_STUB "Use a non-functional nod stub for Android build probes" OFF)

if(MP_ANDROID_NOD_STUB)
    if(NOT ANDROID)
        message(FATAL_ERROR "MP_ANDROID_NOD_STUB is only valid for Android builds")
    endif()
    add_library(mp_android_nod_stub STATIC platform/android_probe/nod_stub.c)
    target_include_directories(mp_android_nod_stub PUBLIC platform/android_probe/include)
    add_library(nod::nod ALIAS mp_android_nod_stub)
endif()

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
# Not portable: raw PowerPC asm (NES emulator wrapper), and decomp-only
# type-matching scaffolding.
list(REMOVE_ITEM MP_GAME_SOURCES
    src/NESemu/modwrapper.cpp
    src/MetroidPrime/TypesMatch.cpp)
# The in-game NES emulator embeds hand-written PowerPC asm; exclude it for now
# and stub its entry points when wiring gameplay.
list(FILTER MP_GAME_SOURCES EXCLUDE REGEX "^src/NESemu/")
# The game entry is included now that it is adapted to Aurora (its `main` is
# renamed to `metroid_main` and driven by platform/main.cpp).
list(APPEND MP_GAME_SOURCES src/MetroidPrime/main.cpp)
# The port has an explicit source manifest, including registration-only and
# shim-dependent translation units. Link those objects directly on both ELF and
# COFF rather than relying on archive extraction or suppressing duplicate symbols.
add_library(mp_game OBJECT ${MP_GAME_SOURCES})
target_include_directories(mp_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/platform/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/aurora/include
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/musyx/include)
target_link_libraries(mp_game PUBLIC
    aurora::core aurora::gx aurora::gd aurora::si aurora::vi aurora::pad
    aurora::mtx aurora::os aurora::dvd aurora::thp aurora::card
    musyx)
# Keep useful native diagnostics; suppress only known decomp syntax warnings.
# compat.h restores SDK macros Aurora omits.
if(MSVC)
    target_compile_options(mp_game PRIVATE /W3
        /FI${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
else()
    target_compile_options(mp_game PRIVATE
        -Wformat -Werror=return-type -Wno-narrowing -Wno-multichar -Wno-write-strings
        -include ${CMAKE_CURRENT_SOURCE_DIR}/platform/compat.h)
endif()

# Port entry point and platform shims (Aurora owns the real main; see
# aurora/main.h). Android loads the target through SDL's Java activity.
set(MP_PORT_SOURCES
    platform/main.cpp
    platform/debug_ui.cpp
    platform/disc.cpp
    platform/shims.cpp
    platform/sdk_stubs.cpp
    platform/port_textures.cpp
    platform/port_prompts.cpp
    platform/glibc_compat.c
    platform/ai_dma.cpp)
if(ANDROID)
    add_library(metroid_prime_port SHARED ${MP_PORT_SOURCES})
    target_link_options(metroid_prime_port PRIVATE "-Wl,-u,SDL_main")
else()
    add_executable(metroid_prime_port ${MP_PORT_SOURCES})
endif()
add_custom_target(port_build_info
    COMMAND ${CMAKE_COMMAND}
        "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DOUTPUT_HEADER=${CMAKE_CURRENT_BINARY_DIR}/generated/port_build_info.h"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/BuildRevision.cmake"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/generated/port_build_info.h"
    VERBATIM)
add_dependencies(metroid_prime_port port_build_info)
target_include_directories(metroid_prime_port PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Keep the HD texture replacements next to the executable, which is where the
# port looks for them by default, so a built binary picks them up without a
# manual copy. Regenerate them with tools/make_prompt_glyphs.py.
add_custom_command(TARGET metroid_prime_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/textures"
        "$<TARGET_FILE_DIR:metroid_prime_port>/textures"
    COMMENT "Copying texture replacements next to the executable")
if(MP_ENABLE_SMOKE_DRIVER)
    target_sources(metroid_prime_port PRIVATE platform/smoke.cpp)
    target_compile_definitions(mp_game PRIVATE MP_ENABLE_SMOKE_DRIVER)
endif()
# aurora_core (webgpu/gpu.cpp) and aurora_gx reference each other; group them so
# the linker rescans and resolves the cycle.
if(MSVC)
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main aurora::core aurora::gx)
    target_link_options(metroid_prime_port PRIVATE /INCREMENTAL:NO)
elseif(CMAKE_LINK_GROUP_USING_RESCAN_SUPPORTED OR CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED)
    target_link_libraries(metroid_prime_port PRIVATE
        mp_game
        aurora::main
        "$<LINK_GROUP:RESCAN,aurora::core,aurora::gx>")
else()
    message(FATAL_ERROR "This port currently requires an ELF linker with RESCAN support or an MSVC-compatible COFF linker")
endif()

include(extern/aurora/cmake/AuroraCopyRuntimeDLLs.cmake)
aurora_copy_runtime_dlls(metroid_prime_port)

# Aurora imports this machine-independent pipeline description cache and
# compiles its entries in the background before first use.
add_custom_command(TARGET metroid_prime_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/assets/initial_pipeline_cache.db
        $<TARGET_FILE_DIR:metroid_prime_port>/initial_pipeline_cache.db)

if(MP_BUILD_TESTS)
    enable_testing()
    add_test(NAME port_version COMMAND metroid_prime_port --version)
    set_tests_properties(port_version PROPERTIES TIMEOUT 15 LABELS port
        PASS_REGULAR_EXPRESSION "Metroid Prime native port")
    add_executable(port_buffer_tests
        tests/port_buffers.cpp
        tests/port_forward_owner.cpp
        src/Kyoto/Streams/CInputStream.cpp
        src/Kyoto/Streams/CMemoryInStream.cpp
        src/Kyoto/Streams/COutputStream.cpp
        src/Kyoto/Streams/CMemoryStreamOut.cpp)
    target_include_directories(port_buffer_tests PRIVATE include extern/aurora/include)
    target_compile_definitions(port_buffer_tests PRIVATE TARGET_PC VERSION=0)
    add_executable(port_ai_tests tests/port_ai.cpp platform/ai_dma.cpp)
    target_include_directories(port_ai_tests PRIVATE extern/aurora/include)
    target_compile_definitions(port_ai_tests PRIVATE TARGET_PC)
    target_link_libraries(port_ai_tests PRIVATE ${AURORA_SDL3_TARGET})
    add_executable(port_musyx_tests tests/port_musyx.c)
    target_link_libraries(port_musyx_tests PRIVATE musyx)
    add_executable(port_disc_tests tests/port_disc.cpp platform/disc.cpp)
    target_include_directories(port_disc_tests PRIVATE platform/include extern/aurora/include)
    target_compile_definitions(port_disc_tests PRIVATE TARGET_PC)
    add_executable(port_mouse_tests tests/port_mouse.cpp)
    target_include_directories(port_mouse_tests PRIVATE platform/include)
    add_executable(port_timing_tests tests/port_timing.cpp)
    target_include_directories(port_timing_tests PRIVATE platform/include)
    add_executable(port_audio_math_tests tests/port_audio_math.c)
    target_include_directories(port_audio_math_tests PRIVATE extern/musyx/include)
    add_executable(port_card_tests tests/port_card.cpp)
    target_link_libraries(port_card_tests PRIVATE aurora::card)
    add_test(NAME port_card_tests COMMAND port_card_tests "${CMAKE_CURRENT_BINARY_DIR}/card-test-data")
    set_tests_properties(port_card_tests PROPERTIES TIMEOUT 15 LABELS port)
    aurora_copy_runtime_dlls(port_card_tests)
    foreach(test IN ITEMS port_buffer_tests port_ai_tests port_musyx_tests port_disc_tests port_mouse_tests port_timing_tests port_audio_math_tests)
        add_test(NAME ${test} COMMAND ${test})
        set_tests_properties(${test} PROPERTIES TIMEOUT 15 LABELS port)
        aurora_copy_runtime_dlls(${test})
    endforeach()
endif()
