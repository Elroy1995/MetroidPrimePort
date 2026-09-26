# Android build probe

## Status

This is an installable 64-bit Android port. It cross-compiles `arm64-v8a`
native code, packages the SDL Java sources, manifest, pipeline cache and
replacement textures, and produces a signed debug APK:

```
android/app/build/outputs/apk/debug/app-debug.apk
```

The Gradle project lives in `android/`, and `tools/android_apk.sh` locates the
SDK and NDK and drives Gradle. The native link was first proven by a feasibility
probe before the packaging layer was added; see [Probe history](#probe-history).

## Requirements

- Android SDK platform 34 (compileSdk / targetSdk 34) and the platform-tools
  `adb`
- CMake 3.31.5, matched by the Android plugin's external native build
- Android NDK r29 (29.0.14206865) or newer - Aurora needs C++20 `std::jthread`
  and `std::stop_token`, which NDK r27's libc++ headers disable
- JDK 17; the Android plugin 8.5.2 and Gradle 8.x require it
- A Gradle 8.x toolchain: set `GRADLE` to a `gradle-8.*` binary, or use the
  wrapper distribution under `~/.gradle/wrapper`
- For a playable build (real `nod`), Rust with the `aarch64-linux-android`
  target. The script uses a vendored Cargo/Rustup under
  `build/android-rust` when present, and the NDK's `aarch64-linux-android28-clang`
  as the linker

## Build the APK

```sh
ANDROID_SDK_ROOT=/path/to/sdk ANDROID_NDK_HOME=/path/to/ndk \
  tools/android_apk.sh :app:assembleDebug -PandroidNodStub=false
```

Set `ANDROID_SDK_ROOT` (the script also accepts `ANDROID_HOME`) and
`ANDROID_NDK_HOME` (also `ANDROID_NDK_ROOT`). The script writes `sdk.dir` and
passes the selected NDK to Gradle, then runs Gradle with `--no-daemon`. The nod
stub is on by default; pass `-PandroidNodStub=false` to build the real `nod`,
which needs the Rust target above.

Optional dependency cache:

```sh
ANDROID_SDK_ROOT=/path/to/sdk ANDROID_NDK_HOME=/path/to/ndk \
  tools/android_apk.sh :app:assembleDebug \
  -PandroidNodStub=false -PcmakeDependencyCache=/absolute/path
```

With `-PcmakeDependencyCache`, each pinned FetchContent package is handed to
CMake via `FETCHCONTENT_SOURCE_DIR_*` pointed at a local directory, so the first
build does not hit the network. Populate that cache with lower-cased
subdirectories named after each pinned package. The ones this build relies on
include `dawn_prebuilt`, `aurora_nod` and `corrosion`, and also `sdl`,
`abseil-cpp`, `xxhash`, `fmt`, `zlib`, `png`, `freetype`, `imgui`, `sqlite3`,
`zstd` and `tracy`. Only directories that exist are used.

The build outputs `android/app/build/outputs/apk/debug/app-debug.apk`. Add
`:app:assembleRelease` instead of `:app:assembleDebug` for the optimized APK at
`android/app/build/outputs/apk/release/app-release.apk`; that is the variant to
play.

## Install and run

```sh
adb install -r android/app/build/outputs/apk/release/app-release.apk
adb shell am start -W -n org.metroidprime.port/.MetroidPrimeActivity
```

The package name is `org.metroidprime.port`; the launcher activity is
`MetroidPrimeActivity`. On first launch the app uses Android's Storage Access
Framework (SAF) to pick the disc: it opens the system document picker, retains a
persistable read URI permission, and feeds the resulting file-descriptor stream
to `nod` without assuming a filesystem path. It accepts only the user's disc:
`GM8E01`, disc 0, revision 0 (verified in `platform/main.cpp`); any other image
is rejected. No game data is bundled, and the app requests no broad storage
permission - it reads only the single persisted document URI. The bundled
`textures` and `initial_pipeline_cache.db` are copied on launch from the APK
assets into private app storage (`files/`).

## Nod stub

`-PandroidNodStub=true` (the default) builds a `nod` ABI shim that provides only
the interface Aurora calls. It cannot read a disc, so the package is not
playable; it exists to isolate link/packaging problems from a reproducible
`nod` cross-build. Use `-PandroidNodStub=false` for a playable build.

## Archipelago and the disc path (2026-09-26)

The APK builds with the Archipelago client compiled in. Two things were found by
building and running it:

- `platform/port_ws.cpp` did not compile for Android at all: Bionic declares
  `IPPROTO_TCP` in `<netinet/in.h>`, which the file did not include, while glibc
  and Winsock both supply it through `<netdb.h>`. Until that include was added
  the whole WebSocket client, and so the whole Archipelago feature, was missing
  from the Android build.
- `wss://` still does not work there. The NDK has no OpenSSL, so the build has
  no `MP_HAVE_OPENSSL` and the client refuses a `wss://` server by design rather
  than downgrading it. A JNI `SSLSocket` backend or a vendored TLS library is
  what that needs; nothing about the port's protocol code blocks it.
- The remembered disc path is Android-specific: `ResolveDiscPath` accepts a
  `content://` URI from the file picker, and the remembered value is handed
  straight to `aurora_dvd_open`. On a device whose saved URI no longer opens -
  the picker grants access to a document, and that grant can lapse - the app
  prints `failed to open disc image: content://…` and exits, and it does the
  same on every later launch, because the failing value is exactly what it
  remembers. That needs a directed fix: fall back to asking again when a
  remembered `content://` URI fails to open, rather than retrying it forever.
  It is not fixed here because it cannot be verified without a device.
- The port's own diagnostics used to go to stderr, which Android discards
  entirely, so a device run that exits during startup reports nothing about why.
  They now go to logcat under the `metroidprime` tag (`PortLog::Write`), which
  is how the disc-path failure above was read on the device. Desktop behaviour
  is unchanged.

## Current limitations

- The first touch overlay provides digital movement and camera sticks plus the
  GameCube face, shoulder, Start and D-pad controls. `HIDE` collapses it to a
  small `SHOW` tab. Its sizing and ergonomics still need device testing;
  physical USB and BLE controllers remain supported.
- On-device behavior is not yet fully exercised: the Vulkan renderer, activity
  lifecycle, audio, controller, and SAF/disc-access paths have been wired up but
  still need device verification.
- Build an optimized APK to play. The `debug` variant compiles the native code
  with `-O0` and is far too slow; `:app:assembleRelease` builds `RelWithDebInfo`.
  It is signed with this project's own key once one exists
  (`tools/make_android_keystore.sh`); until then `tools/android_apk.sh` signs
  with the debug key for a local sideload and says so, and the build itself
  refuses that unless it is asked for — see `docs/RELEASING.md`. Measured on a
  POCO F8 Ultra, the release build holds a steady 60 FPS where the debug build
  stutters badly.
- The memory card lives in app storage, so saves survive reinstall of the same
  data and need no storage permission.
- **There is no "unidentified" card bug.** The report was a misreading of
  Metroid Prime's opening narration: the game displays "Unidentified distress
  beacon has been transmitted" as part of its story setup, and that sentence is
  not about the card. Captured and confirmed — see `PORT_NOTES.md`. The card
  itself is found, and a save on it is opened, both on desktop and through the
  same code path Android uses.
- **Saving works**, verified end to end on that same path: a real save is written
  to the card carrying its comment and a timestamp, and the next boot opens it
  without reporting corruption. Only slot B fails, which is correct — nothing has
  ever been written there. Loading a save from the **title screen's Continue** is
  still unverified; see `PORT_NOTES.md`.
- The port logs the directory it resolved the card to on every platform, so a
  device run can confirm where saves go in one line:
  `memory card: storing under <path>`. The same code falls back to the app's
  internal storage if `SDL_GetPrefPath` cannot answer, instead of letting the
  card land in the process's working directory, which on Android is not
  writable.
- Distribution obligations remain. Aurora and MusyX are MIT snapshots and the
  button prompt icons are Kenney CC0; if any GPL-covered code ends up in the
  final combined work, the required corresponding source and build material must
  be provided under the applicable GPL terms. The separate GPL recompilation
  toolchain is not part of this native link. The port's own work is MIT — see
  `LICENSE` and `NOTICE`, which also state that the grant does not cover the
  decompiled game code. Never package a disc image or extracted copyrighted
  game assets.

## Probe history

The native target was first proven as a feasibility scaffold that configured and
linked `arm64-v8a` as `build/android-aarch64/libmetroid_prime_port.so`. That
probe required NDK r29 or newer and used `build/review-tools/bin/cmake` and
`ninja`:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk-r29 tools/android_probe.sh
```

It completed on 2026-09-21 with Android NDK r29 (Clang 21.0.0). CMake selected
Aurora's pinned `dawn-android-aarch64` package, built vendored SDL 3.4.10 with
its Android video, audio, input and Vulkan backends, and completed the 1,243-step
native build graph. The result was verified as:

```text
ELF 64-bit LSB shared object, ARM aarch64, dynamically linked,
for Android 28, built by NDK r29
```

Its dynamic dependencies are Android platform libraries (`libc`, `libm`,
`libz`, `libandroid`, `liblog`, OpenSL ES, GLES, and `libdl`); no host library
leaked into the result. Two build-system issues were found and addressed:

- Aurora's provider accepted host `pkg-config` results for libpng and Freetype
  during cross-compilation. The provider now skips pkg-config fallbacks when
  `CMAKE_CROSSCOMPILING` and builds the pinned sources instead.
- An SDL Android app is a JNI-loaded shared library, not a standalone ELF.
  `metroid_prime_port` is therefore a shared library only when `ANDROID` is set;
  desktop targets remain executables.

GitHub archive downloads returned transient HTTP 504 responses during that run.
Matching pinned source checkouts were supplied through CMake's
`FETCHCONTENT_SOURCE_DIR_*` overrides to finish the probe. This was a network
failure rather than an Android build incompatibility and no local cache paths are
embedded in the scaffold.

At that point the `nod` source build could not be validated: Aurora fetches
`nod-ffi` (nod 2.0.0-alpha.12 and Corrosion 0.6.1) and builds it with Cargo, but
Cargo and the Android Rust target were not installed. The stub provided only the
nod ABI and always failed disc access. Both gaps are now covered by the APK
build above.
