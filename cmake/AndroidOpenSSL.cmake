# OpenSSL for the Android build, which gives the Archipelago client wss://.
#
# The NDK ships no TLS library, and the libcrypto on a device is Android's
# private BoringSSL, which an app must not link. So OpenSSL is built here from a
# pinned release and linked statically. Its build is Perl and make, not CMake,
# so it runs as an ExternalProject; the source is fetched at configure time so
# that its version can be checked there, and so that FETCHCONTENT_SOURCE_DIR_OPENSSL
# points an offline build at a local copy the way it does for every other
# fetched package.
#
# Defines OpenSSL::SSL and OpenSSL::Crypto, the targets find_package(OpenSSL)
# would, plus OpenSSL_FOUND, OPENSSL_VERSION and MP_OPENSSL_BUILD_TARGET.

set(MP_OPENSSL_VERSION 3.5.8)
set(MP_OPENSSL_SHA256 a8f84a39918ec6415ce765d9b429d313ba97b8143169c172e734b9514464f5b2)

find_package(Perl)
find_program(MP_OPENSSL_MAKE NAMES gmake make)
if(NOT PERL_FOUND OR NOT MP_OPENSSL_MAKE)
    message(FATAL_ERROR
        "building OpenSSL for Android needs Perl and make on the build machine "
        "(perl: ${PERL_EXECUTABLE}, make: ${MP_OPENSSL_MAKE}).\n"
        "Without it wss:// cannot work on Android. Install them, or pass "
        "-DMP_ALLOW_NO_TLS=ON to build without TLS on purpose.")
endif()

include(FetchContent)
# SOURCE_SUBDIR names a directory that does not exist, so MakeAvailable fetches
# the source without trying to add it as a CMake project.
FetchContent_Declare(openssl
    URL "https://github.com/openssl/openssl/releases/download/openssl-${MP_OPENSSL_VERSION}/openssl-${MP_OPENSSL_VERSION}.tar.gz"
    URL_HASH SHA256=${MP_OPENSSL_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    SOURCE_SUBDIR mp-not-a-cmake-project)
FetchContent_MakeAvailable(openssl)

# A local copy from FETCHCONTENT_SOURCE_DIR_OPENSSL skips the hash check, so the
# version is checked from the source itself.
file(STRINGS "${openssl_SOURCE_DIR}/VERSION.dat" mp_openssl_version_lines REGEX "^(MAJOR|MINOR|PATCH)=")
set(mp_openssl_major "")
set(mp_openssl_minor "")
set(mp_openssl_patch "")
foreach(line IN LISTS mp_openssl_version_lines)
    if(line MATCHES "^(MAJOR|MINOR|PATCH)=([0-9]+)")
        string(TOLOWER "${CMAKE_MATCH_1}" part)
        set(mp_openssl_${part} "${CMAKE_MATCH_2}")
    endif()
endforeach()
if(NOT mp_openssl_major STREQUAL "3" OR NOT mp_openssl_minor STREQUAL "5")
    message(FATAL_ERROR "OpenSSL source at ${openssl_SOURCE_DIR} is version "
        "'${mp_openssl_major}.${mp_openssl_minor}.${mp_openssl_patch}'; the Android build needs 3.5")
endif()
set(OPENSSL_VERSION "${mp_openssl_major}.${mp_openssl_minor}.${mp_openssl_patch}")

# The notice travels in the APK: android/app/build.gradle collects it from here,
# which exists whether or not the source came from the dependency cache.
file(COPY "${openssl_SOURCE_DIR}/LICENSE.txt" DESTINATION "${CMAKE_BINARY_DIR}/_deps/openssl-license")

if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(mp_openssl_target android-arm64)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(mp_openssl_target android-arm)
elseif(ANDROID_ABI STREQUAL "x86_64")
    set(mp_openssl_target android-x86_64)
elseif(ANDROID_ABI STREQUAL "x86")
    set(mp_openssl_target android-x86)
else()
    message(FATAL_ERROR "no OpenSSL target for Android ABI '${ANDROID_ABI}'")
endif()
if(ANDROID_PLATFORM_LEVEL)
    set(mp_openssl_api ${ANDROID_PLATFORM_LEVEL})
else()
    set(mp_openssl_api ${CMAKE_SYSTEM_VERSION})
endif()
# OpenSSL's Android configuration finds clang on PATH and the NDK through
# ANDROID_NDK_ROOT. The compiler CMake uses is the NDK's own, so its directory
# is the toolchain's bin.
get_filename_component(mp_ndk_bin "${CMAKE_C_COMPILER}" DIRECTORY)
if(ANDROID_NDK)
    set(mp_ndk_root "${ANDROID_NDK}")
else()
    set(mp_ndk_root "${CMAKE_ANDROID_NDK}")
endif()

set(mp_openssl_prefix "${CMAKE_BINARY_DIR}/openssl-android")
set(mp_openssl_env ${CMAKE_COMMAND} -E env
    "ANDROID_NDK_ROOT=${mp_ndk_root}"
    "PATH=${mp_ndk_bin}:$ENV{PATH}")
include(ProcessorCount)
ProcessorCount(mp_jobs)
if(mp_jobs EQUAL 0)
    set(mp_jobs 4)
endif()

include(ExternalProject)
# Only the two libraries: no command-line tool, tests, docs, legacy provider,
# engines or runtime-loaded modules. OPENSSLDIR is never read for certificates
# on Android - port_ws.cpp loads the system store itself - so its default is
# left alone.
ExternalProject_Add(openssl_android
    SOURCE_DIR "${openssl_SOURCE_DIR}"
    BINARY_DIR "${CMAKE_BINARY_DIR}/openssl-android-build"
    INSTALL_DIR "${mp_openssl_prefix}"
    CONFIGURE_COMMAND ${mp_openssl_env} "${PERL_EXECUTABLE}" "${openssl_SOURCE_DIR}/Configure"
        ${mp_openssl_target} -D__ANDROID_API__=${mp_openssl_api}
        "--prefix=${mp_openssl_prefix}" --libdir=lib
        no-shared no-apps no-tests no-docs no-legacy no-engine no-dso
        -fPIC -ffunction-sections -fdata-sections
    BUILD_COMMAND ${mp_openssl_env} "${MP_OPENSSL_MAKE}" -j${mp_jobs} build_libs
    INSTALL_COMMAND ${mp_openssl_env} "${MP_OPENSSL_MAKE}" install_dev
    BUILD_BYPRODUCTS
        "${mp_openssl_prefix}/lib/libssl.a"
        "${mp_openssl_prefix}/lib/libcrypto.a"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
    LOG_INSTALL TRUE
    LOG_OUTPUT_ON_FAILURE TRUE)

# The include directory has to exist when the imported targets are declared,
# which is before the ExternalProject has installed anything into it.
file(MAKE_DIRECTORY "${mp_openssl_prefix}/include")
add_library(OpenSSL::Crypto STATIC IMPORTED)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${mp_openssl_prefix}/lib/libcrypto.a"
    INTERFACE_INCLUDE_DIRECTORIES "${mp_openssl_prefix}/include")
add_library(OpenSSL::SSL STATIC IMPORTED)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${mp_openssl_prefix}/lib/libssl.a"
    INTERFACE_INCLUDE_DIRECTORIES "${mp_openssl_prefix}/include"
    INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
set(OpenSSL_FOUND TRUE)
set(MP_OPENSSL_BUILD_TARGET openssl_android)
