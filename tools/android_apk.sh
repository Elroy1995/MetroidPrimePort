#!/bin/sh
set -eu

root_dir=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
home=${HOME:-/nonexistent}
sdk=${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$home/android/sdk}}
ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}

if [ ! -d "$sdk" ]; then
    echo "error: Android SDK not found; set ANDROID_SDK_ROOT" >&2
    exit 1
fi
if [ -z "$ndk" ]; then
    for candidate in "$sdk"/ndk/* "$home"/android-ndk-cache/android-ndk-*; do
        if [ -f "$candidate/source.properties" ]; then
            ndk=$candidate
        fi
    done
fi
if [ -z "$ndk" ] || [ ! -f "$ndk/source.properties" ]; then
    echo "error: Android NDK not found; set ANDROID_NDK_HOME" >&2
    exit 1
fi
ANDROID_NDK_HOME=$ndk
export ANDROID_NDK_HOME

gradle=${GRADLE:-}
if [ -z "$gradle" ]; then
    gradle=$(command -v gradle || true)
fi
if [ -z "$gradle" ]; then
    for candidate in "$home"/.gradle/wrapper/dists/gradle-8.*/*/gradle-8.*/bin/gradle; do
        if [ -x "$candidate" ]; then
            gradle=$candidate
        fi
    done
fi
if [ -z "$gradle" ]; then
    echo "error: Gradle 8.x not found; set GRADLE" >&2
    exit 1
fi

repo_cargo="$root_dir/build/android-rust/cargo"
repo_rustup="$root_dir/build/android-rust/rustup"
if [ -x "$repo_cargo/bin/cargo" ]; then
    CARGO_HOME=$repo_cargo
    RUSTUP_HOME=$repo_rustup
    PATH="$repo_cargo/bin:$PATH"
    export CARGO_HOME RUSTUP_HOME PATH
fi

ndk_bin="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$ndk_bin/aarch64-linux-android28-clang"
CC_aarch64_linux_android=$CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER
CXX_aarch64_linux_android="$ndk_bin/aarch64-linux-android28-clang++"
AR_aarch64_linux_android="$ndk_bin/llvm-ar"
RANLIB_aarch64_linux_android="$ndk_bin/llvm-ranlib"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER CC_aarch64_linux_android
export CXX_aarch64_linux_android AR_aarch64_linux_android RANLIB_aarch64_linux_android

escape_property() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/:/\\:/g; s/ /\\ /g'
}

{
    printf 'sdk.dir=%s\n' "$(escape_property "$sdk")"
} > "$root_dir/android/local.properties"

exec "$gradle" --project-dir "$root_dir/android" --no-daemon "$@"
