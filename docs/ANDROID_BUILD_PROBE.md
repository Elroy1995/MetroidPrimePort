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

## Running the arm64 APK on the x86_64 emulator

The APK is `abiFilters "arm64-v8a"` only, and the machine's Android image is
x86_64. It still works: Android 11+ x86_64 system images carry ARM translation,
so the real arm64 APK installs and runs.

```sh
export ANDROID_HOME=~/android/sdk
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
~/android/sdk/emulator/emulator -avd dkc1 -no-window -no-audio -no-boot-anim \
  -gpu swiftshader_indirect
adb wait-for-device
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am start -W -n org.metroidprime.port/.MetroidPrimeActivity
adb logcat -d | grep -i 'metroidprime\|aurora\|vulkan'
```

The AVD is `dkc1` (API 34, `google_apis`, x86_64). It boots in about 40 s on
KVM. Confirm translation is present before trusting a result:

```sh
adb shell getprop ro.dalvik.vm.native.bridge      # libndk_translation.so
adb shell getprop ro.product.cpu.abilist          # x86_64,arm64-v8a
```

**What this does and does not prove.** It proves the APK is well-formed, links,
loads its native library, brings up WebGPU, and has a working Java/SAF
lifecycle — `SDL_main` runs from `lib/arm64/libmetroid_prime_port.so`, the Dawn
cache and pipeline cache are seeded, and on a fresh install the app correctly
opens the SAF document picker and waits. It does **not** measure performance:
everything is ARM-translated onto an x86_64 CPU and rendered by SwiftShader, so
frame rates say nothing about a real phone. Nor does it prove a real GPU driver
behaves, or that touch input feels right. Use it for lifecycle, packaging and
network questions; use a device for performance and input.

One benign line is expected and is not an error:

```
E/ndk_translation: Unknown function is used with vkGetInstanceProcAddr:
  vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR
```

## wss:// on Android, proven end to end

`wss://` works in the APK. OpenSSL 3.5.8 is built from source for the NDK by
`cmake/AndroidOpenSSL.cmake` — pinned version and SHA-256, `no-shared`, so it is
linked statically — and `MP_HAVE_OPENSSL` is defined. OpenSSL is Apache-2.0; its
`LICENSE.txt` is copied into the APK by the existing `syncLicenseNotices` task.

`find_package(OpenSSL)` stays optional on the desktop but is **required** on
Android, because a lookup that quietly finds nothing is how an APK ends up
refusing every `wss://` server. `-DMP_ALLOW_NO_TLS=ON` builds without it on
purpose. Perl and `make` must be on the machine that builds the APK;
`cmake/AndroidOpenSSL.cmake` stops with that message if they are not.

To reproduce the whole thing, including the proof:

```sh
# 1. A CA and a server certificate for 127.0.0.1, so the hostname check holds.
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.pem -days 2 \
  -subj "/CN=MP Test CA"
openssl req -newkey rsa:2048 -nodes -keyout srv.key -out srv.csr -subj "/CN=127.0.0.1"
printf 'subjectAltName=IP:127.0.0.1,DNS:localhost\n' > san.ext
openssl x509 -req -in srv.csr -CA ca.pem -CAkey ca.key -CAcreateserial \
  -out srv.pem -days 2 -extfile san.ext

# 2. The fake server. The device reaches the host's loopback through adb
#    reverse, so the certificate's 127.0.0.1 SAN still applies.
python3 tools/ap_fake_server.py --port 38391 --slot P --tls \
  --cert srv.pem --key srv.key &
adb reverse tcp:38391 tcp:38391

# 3. The app reads its config from its own private files directory.
adb push ca.pem /data/local/tmp/ && adb push archipelago.json /data/local/tmp/
adb shell "run-as org.metroidprime.port cp /data/local/tmp/ca.pem \
  /data/data/org.metroidprime.port/files/ca.pem"
adb shell "run-as org.metroidprime.port cp /data/local/tmp/archipelago.json \
  /data/data/org.metroidprime.port/files/archipelago.json"

# 4. Run it. The pass condition is a completed handshake, not a clean build.
adb shell am start -n org.metroidprime.port/.MetroidPrimeActivity
adb logcat -d | grep -i archipelago
```

`archipelago.json` is `{"server":"wss://127.0.0.1:38391","slot":"P",
"tls_ca":"/data/data/org.metroidprime.port/files/ca.pem", ...}`.

Two results, and the second is the one that matters:

```
archipelago: connecting to wss://127.0.0.1:38391
archipelago: connected as P                        <- a real wss:// session
```

and with `tls_ca` removed, so the **system** store is used:

```
archipelago: TLS certificate verification failed: unable to get local issuer certificate
```

That second one is a pass, not a failure. The test server's CA is not in
Conscrypt's store, so a correct outcome is a verification error naming the
issuer. The failure it rules out is `no TLS root certificates: loaded 0 from
/apex/com.android.conscrypt/cacerts ...`, which would mean the enumeration found
nothing. Getting the verification error proves the store was read, populated and
offered to the server.

### Checking the built library

Link statically with `--exclude-libs,libssl.a:libcrypto.a`, which is narrower
than `ALL` on purpose: excluding `ALL` would also hide the JNI entry points in
the static SDL archive, which Java looks up by name. On a stripped release `.so`,
`nm -D` will not show `SSL_connect`, so check strings instead:

```sh
unzip -p android/app/build/outputs/apk/release/app-release.apk \
  lib/arm64-v8a/libmetroid_prime_port.so > /tmp/port.so
strings /tmp/port.so | grep -m1 '^OpenSSL 3\.'
strings /tmp/port.so | grep -c 'built without OpenSSL'          # must be 0
strings /tmp/port.so | grep -c 'no TLS root certificates'       # must be > 0
llvm-readelf -d /tmp/port.so | grep -cE 'NEEDED.*lib(ssl|crypto)\.so'   # must be 0
```

All four were checked on the release APK. The `NEEDED` check is the important
one: a dynamic link would build cleanly and then fail on a device, because the
system `libcrypto` is private BoringSSL that an app may not use.

The release APK is 13.46 MB with OpenSSL and 11.19 MB without, so **+2.17 MB,
+20.3%** — more than double the 0.8–1.2 MB first estimated for this.

## The Conscrypt trust store, verified readable

This was the open question behind the Android `wss://` plan, and it is settled.
Android's `SSL_CTX_set_default_verify_paths` points at a compiled-in
`OPENSSLDIR` that does not exist, returns success, and loads nothing — so the
port's "could not load the system TLS trust store" check never fires and every
public server then fails obscurely. The port must enumerate the certificates
itself. From inside the app's own sandbox on API 34:

```sh
adb shell run-as org.metroidprime.port ls /apex/com.android.conscrypt/cacerts | wc -l
# 134
adb shell "run-as org.metroidprime.port head -c 4 \
  /apex/com.android.conscrypt/cacerts/\$(ls /apex/com.android.conscrypt/cacerts | head -1)"
# ----            i.e. the start of -----BEGIN CERTIFICATE-----
```

So the app can both list and read them, under SELinux: the files carry
`u:object_r:system_security_cacerts_file:s0` and the app runs in
`untrusted_app`. `/system/etc/security/cacerts` holds the same 134 as a
fallback. The names are 8 hex digits plus `.0` (`01419da9.0`).

That naming is why the loader must read each file directly rather than hand the
directory to OpenSSL: hashed-directory lookup is lazy, so there is nothing to
count and nothing to fail on, and a hash-convention mismatch would surface only
as a verification error — the same silent trap as above. Enumerating also makes
it possible to **fail loudly when zero certificates load**, naming the
directories tried. On API 34+ the APEX copy is authoritative and replaces the
`/system` one, so use the first directory that yields at least one certificate
rather than merging them.

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
- **A short tap on the overlay can be missed.** SDL's virtual joystick is
  state-sampling, not event-queueing: a press and release that both land between
  two joystick updates leave only the release, so the game never sees the press.
  A quick tap on A or Start can therefore do nothing, most visibly while a game
  frame is stalled. It is not a race — the setters hold SDL's joystick mutex — and
  sustained presses and ordinary releases are unaffected, which is why it never
  showed up as "the controls don't work". Fixing it means latching a press until
  the game samples it, released on an update the port does not control, so it is
  recorded at the code rather than half-solved. Worth checking on a device: tap
  Start rapidly and confirm the game menu opens every time.
- The pad is now built from `platform/touch_pad.cpp` and covered by
  `port_touch_pad_tests`, which attaches a real SDL virtual gamepad and asserts
  what the game reads back. That is the first time any of this code has been
  compiled outside an Android build; it previously had no test at all.
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
