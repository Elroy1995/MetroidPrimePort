# Android build probe

This is a feasibility scaffold, not an installable Android port. It configures
and links the native target for 64-bit Android as
`build/android-aarch64/libmetroid_prime_port.so`. A Gradle project is not needed
to prove the native link; it will be needed to package the library, SDL Java
sources, manifest, assets, and application resources into an APK.

## Run the probe

The probe requires NDK r29 or newer because Aurora uses C++20 `std::jthread`
and `std::stop_token`. NDK r27's libc++ headers disable those APIs. Set the NDK
explicitly when it is not under a standard SDK directory:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r29 tools/android_probe.sh
```

The script uses `build/review-tools/bin/cmake` and `ninja`, targets
`arm64-v8a` at API 28, and fetches Aurora's pinned dependencies on the first
configure. `ANDROID_BUILD_DIR` can select another build directory. Additional
arguments are passed to the CMake configure command.

The equivalent build step after configuration is:

```sh
build/review-tools/bin/cmake --build build/android-aarch64 \
  --target metroid_prime_port --parallel
```

## Probe result

The probe completed on 2026-09-21 with Android NDK r29 (Clang 21.0.0). CMake
selected Aurora's pinned `dawn-android-aarch64` package, built vendored SDL
3.4.10 with its Android video, audio, input, and Vulkan backends, compiled the
port and dependencies, and completed the 1,243-step native build graph. The
result was verified as:

```text
ELF 64-bit LSB shared object, ARM aarch64, dynamically linked,
for Android 28, built by NDK r29
```

Its dynamic dependencies are Android platform libraries (`libc`, `libm`,
`libz`, `libandroid`, `liblog`, OpenSL ES, GLES, and `libdl`); no host library
leaked into the result.

Two build-system issues were found and addressed by the scaffold:

- Aurora's provider accepted host `pkg-config` results for libpng and Freetype
  during cross-compilation. The provider now skips pkg-config fallbacks when
  `CMAKE_CROSSCOMPILING` and builds the pinned sources instead.
- An Android SDL application is a JNI-loaded shared library, not a standalone
  ELF executable. `metroid_prime_port` is therefore a shared library only when
  `ANDROID` is set; desktop targets remain executables.

GitHub archive downloads returned transient HTTP 504 responses during this
run. Matching pinned source checkouts were supplied through CMake's
`FETCHCONTENT_SOURCE_DIR_*` overrides to finish the probe. This was a network
failure rather than an Android build incompatibility and no local cache paths
are embedded in the scaffold.

## nod limitation

Aurora has no prebuilt nod package for Android. Its `vendor` provider fetches
nod 2.0.0-alpha.12 and Corrosion 0.6.1, then builds the `nod-ffi` Rust crate.
That path requires Cargo, an Android Rust target such as
`aarch64-linux-android`, and NDK linker configuration. Cargo and Rust were not
installed on the probe machine, so the source build could not be validated.

`MP_ANDROID_NOD_STUB=ON` provides only the nod ABI used by Aurora and always
fails disc access. It exists solely to prove the rest of the native link and
must not be used for a playable package. A real port must either make the
Corrosion cross-build reproducible or ship a matching Android nod library.

## Remaining Android work

- Create an application module from SDL's `android-project` scaffolding. Add
  `AURORA_SDL3_JAVA_SOURCE_DIR` and `AURORA_ANDROID_JAVA_SOURCE_DIR`, load
  `libmetroid_prime_port.so`, package the pipeline cache and replacement
  textures, and exercise lifecycle, Vulkan, audio, and controller behavior on
  a device.
- Replace path-only disc selection with Android's Storage Access Framework.
  Request a document URI, retain its permission, open a file descriptor or
  callback stream, and pass that stream to nod without assuming a filesystem
  path.
- Add touch controls and map them into the existing SDL input path while
  retaining physical-controller support.
- Move saves, configuration, logs, and caches to Android app storage using SDL
  preference/storage paths. Do not assume a writable current directory or
  broad external-storage access.
- Resolve distribution licensing before publishing an APK. Preserve all
  third-party notices. If any GPL-covered code is distributed in the final
  combined work, provide the required corresponding source and build material
  under the applicable GPL terms. The separate GPL recompilation toolchain is
  not part of this native link, Aurora and MusyX are MIT snapshots, and this
  repository currently has no top-level license grant. Never package a disc
  image or extracted copyrighted game assets.
