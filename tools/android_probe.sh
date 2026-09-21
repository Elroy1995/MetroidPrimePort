#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cmake="$root_dir/build/review-tools/bin/cmake"
ninja="$root_dir/build/review-tools/bin/ninja"
build_dir=${ANDROID_BUILD_DIR:-$root_dir/build/android-aarch64}
home=${HOME:-/nonexistent}

if [ ! -x "$cmake" ] || [ ! -x "$ninja" ]; then
    echo "error: build/review-tools/bin/cmake and ninja are required" >&2
    exit 1
fi

ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -z "$ndk" ]; then
    for sdk in "${ANDROID_SDK_ROOT:-}" "${ANDROID_HOME:-}" "$home/Android/Sdk" "$home/android/sdk" /opt/android-sdk; do
        [ -n "$sdk" ] || continue
        for candidate in "$sdk"/ndk/*; do
            if [ -f "$candidate/build/cmake/android.toolchain.cmake" ]; then
                ndk=$candidate
            fi
        done
    done
    for candidate in "$home"/android-ndk-cache/android-ndk-*; do
        if [ -f "$candidate/build/cmake/android.toolchain.cmake" ]; then
            ndk=$candidate
        fi
    done
fi

toolchain=${ndk:+$ndk/build/cmake/android.toolchain.cmake}
if [ -z "$ndk" ] || [ ! -f "$toolchain" ]; then
    echo "error: Android NDK not found; set ANDROID_NDK_HOME" >&2
    exit 1
fi

ndk_revision=
while IFS='=' read -r key value; do
    case $key in
        "Pkg.Revision "*) ndk_revision=${value# } ;;
    esac
done < "$ndk/source.properties"
ndk_major=${ndk_revision%%.*}
if [ -z "$ndk_major" ] || [ "$ndk_major" -lt 29 ]; then
    echo "error: Android NDK r29 or newer is required (found ${ndk_revision:-unknown})" >&2
    exit 1
fi

"$cmake" -S "$root_dir" -B "$build_dir" -G Ninja \
    -DCMAKE_MAKE_PROGRAM="$ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM=android-28 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DMP_BUILD_TESTS=OFF \
    -DMP_ANDROID_NOD_STUB=ON \
    -DAURORA_SDL3_PROVIDER=vendor \
    -DAURORA_SDL3_LINKAGE=static \
    -DAURORA_DAWN_PROVIDER=auto \
    -DAURORA_DAWN_LINKAGE=static \
    "$@"

exec "$cmake" --build "$build_dir" --target metroid_prime_port --parallel
