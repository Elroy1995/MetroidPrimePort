# Handoff

Orientation for whoever picks this up next. `docs/NATIVE_PORT.md` is still the
detailed reference for flags, settings and smoke hooks; this is the map.

## Where things are

- This checkout, branch `port`. HEAD is a moving target — check `git log`.
- `origin` is the upstream decompilation (`PrimeDecomp/prime`); `user` is the
  distribution repo (`Odrannnn/MetroidPrimePort`). CI runs on the `port` branch
  of `user`, and that is where it gets pushed.
- Published builds live in `/home/odran/mport/`: the Linux binary, the AppImage,
  `textures/`, and a copy of the disc image used for local testing.
- There is no system CMake or Ninja. Use `build/review-tools/bin/cmake`,
  `build/review-tools/bin/ctest` and `build/review-tools/bin/ninja`.

## Desktop build and test

```sh
build/review-tools/bin/cmake --build build/port-gcc -j 8
build/review-tools/bin/ctest --test-dir build/port-gcc -L port
```

`build/port-asan` is the same build with the opt-in smoke driver
(`MP_ENABLE_SMOKE_DRIVER=ON`), which the `MP_SMOKE_*` hooks require.

CI (`.github/workflows/`): `native-linux.yml` (GCC 14 and clang 18) and
`windows.yml` (clang-cl, plus packaging and a packaged no-disc startup check).
Both are green. `build.yml` belongs to the upstream decompilation, not to this
port.

Running it needs a disc image; see `docs/NATIVE_PORT.md` for `MP_DISC`, the
first-launch picker, and the `MP_*` flags. `MP_USER_PATH` and `MP_CACHE_PATH`
keep test runs from touching real saves and settings.

## Android

The port runs on Android. The toolchain lives outside the repo:

```sh
source /home/odran/android/env.sh            # JAVA_HOME, ANDROID_HOME, PATH
/home/odran/android/gradle-dist/bin/gradle -p android :app:assembleRelease
/home/odran/android/sdk/platform-tools/adb install -r \
    android/app/build/outputs/apk/release/app-release.apk
```

- Package `org.metroidprime.port`, version 0.1.0. Release APK is ~12 MB.
- There is one test phone, serial `3d929981` (model `25102PCBEG`). It answers as
  both `192.168.68.64:5555` (LAN) and `100.113.164.114:5555` (Tailscale) — the
  same device, not two. A USB device may also be listed; it does not have the app.
- The APK embeds the game code, so any engine change needs a rebuild before it
  can be tested on the phone. Installing stops the running app.

## Packaging

- `tools/make_appimage.sh` builds the AppImage and fetches appimagetool on first
  use. It is linked against glibc 2.39, so it needs Ubuntu 24.04 or newer;
  `platform/glibc_compat.c` is what keeps the floor there rather than at the
  build host's glibc. The remaining requirement above that comes from `nod`'s
  prebuilt Rust library.
- `tools/make_flatpak.sh` and `flatpak/` exist but have never been built here
  (flatpak is not installed). A Flatpak takes glibc from its runtime, which
  sidesteps the AppImage's floor.

## What works

- Linux and Windows builds, CI green on both.
- Gameplay, rendering, audio (AI and Musyx), input, memory-card saves.
- Widescreen: 4:3 / 16:9 / follow-window, with the in-game HUD moved out to the
  wide corners and the pause, map and front-end screens aspect-matched.
- High-FPS support: fixed or adaptive simulation rate with presentation
  decoupled from it.
- Mouse aim, twin-stick, and gyro aiming from a pad or the phone's sensor.
- Rebinding (Controls tab), and in-game button prompts that follow the bound
  input rather than a fixed device icon.
- HD texture replacements, including per-device sets and the prompt glyphs.
- Debug overlay: performance, render, audio, voices, input and a debug tab with
  abilities, teleport and world selection. Reachable on Android via MENU.
- Android: APK, touch controls, touch overlay, high-density display support,
  disc picker.

## Known gaps and unverified things

- The wide-HUD spread now rotates each element rigidly about the eye instead of
  sliding it sideways (that sliding sheared off-axis elements, badly so on a
  20:9 phone). It is built, tested and installed on the phone, but nobody has
  looked at it on the device yet. If an element still distorts, the next suspect
  is a HUD camera that never opts into aspect matching.
- The pad prompt path reads the controller's own button mapping, but that has
  only been exercised through the keyboard path and the no-pad fallback.
- A thin left strip on aspects wider than 16:9 in follow-window mode was seen
  once and never reproduced locally.
- Pause-screen dark Samus legs at fractional EFB scale: a candidate fix exists
  but was never confirmed against the failing setup.
- Water-landing damping at `CGroundMovement.cpp:765` was left alone; it may be a
  one-time landing response rather than a rate bug.
- Windows emits ~238 `-Wmismatched-tags` warnings (classes forward-declared as
  `struct` and defined as `class`, or the reverse). Clang warns these can cause
  MSVC linker errors, which is exactly the family of bug that broke the Windows
  build twice — worth a cleanup pass.
- The pad texture sets carry no Z icon, so a pad whose mapping cannot be read
  falls back to the GameCube art for that prompt.

## Conventions

- `AGENTS.md` governs: never commit disc images, extracted assets or generated
  game code; commit only once a change works, with `port:`, `fix:` or `docs:`
  messages matching the existing style; do not push unless asked.
- After a successful build, copy the binary to `/home/odran/mport/` and rebuild
  after committing so `--version` reports the clean revision.
- `extern/aurora` and `extern/musyx` are vendored snapshots tracked in place;
  Aurora is MIT and Musyx is MIT, and their licences are copied into release
  packages. Keep the GPL note in `AGENTS.md` in mind for distributed artifacts.

## Where to look first

- `docs/NATIVE_PORT.md` — every flag, setting, smoke hook and platform note.
- `platform/` — entry point, disc handling, texture replacements, prompt icons,
  debug overlay, timing.
- `src/GuiSys/` — GUI cameras and frames, including the widescreen HUD spread.
- `src/MetroidPrime/` — the recompiled game.
- `tools/make_prompt_glyphs.py` and `tools/prompt_icons/` — the prompt icons and
  the CC0 pack they come from.
