# Metroid Prime — native port

A native build of **Metroid Prime** (GameCube, `GM8E01_00`, USA v1.00) for Linux
and Windows. The game is statically recompiled from the retail executable and
runs directly on the GPU through Aurora, with no emulator involved.

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets.

## What it adds over the console release

- Widescreen rendering — 4:3, 16:9 or following the window — with the HUD spread
  to the edges and the menus, pause and map screens aspect-corrected
- An uncapped presentation rate over a fixed-rate simulation, so physics and
  animation stay console-accurate at any frame rate
- Mouse aim and twin-stick aiming, with sensitivity and inversion
- A Controls tab for rebinding keyboard, mouse and controller
- HD texture replacements, including in-game button prompts that follow the
  input actually bound to each action
- An F1 debug overlay: performance, render, audio, voices, input, and a debug
  tab with abilities, teleport and world selection

## Building

Needs CMake 3.25+, Ninja and a C++20 toolchain. Aurora and MusyX are vendored, so
a normal clone is enough; Aurora fetches its pinned dependencies on the first
configure.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native -j 4
ctest --test-dir build/native -L port --output-on-failure
```

On Windows, run from an MSVC developer shell and add
`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`. CI builds and tests
Linux (GCC 14 and Clang 18) and Windows (clang-cl); see
`.github/workflows/`.

## Running

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

The disc can also be set with `MP_DISC`, kept beside the executable, or picked
through a file dialog the first time you launch without one — the choice is
remembered in the settings. The image must be `GM8E01`, disc 0, revision 0.

`--version` prints the source revision without starting graphics; include it in
bug reports. The F1 overlay holds the rest of the settings, and the same flags
are available as `MP_*` environment variables.

## Packaging

`tools/make_appimage.sh` builds a self-contained AppImage and
`tools/make_flatpak.sh` a Flatpak. Neither bundles the disc. See
`docs/NATIVE_PORT.md` for what each one expects from the host.

A first Android port builds an installable debug APK for `arm64-v8a`; see
`docs/ANDROID_BUILD_PROBE.md`. Android is a development probe, not a
production-supported target yet.

## Credits and licensing

This is a fork of the [PrimeDecomp/prime](https://github.com/PrimeDecomp/prime)
decompilation, which the game logic is built from. Aurora (`extern/aurora`) and
MusyX (`extern/musyx`) are MIT-licensed vendored snapshots, and the button
prompt icons come from Kenney's Input Prompts pack (CC0), vendored under
`tools/prompt_icons`. Their licences are kept alongside them and are copied into
release packages.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.

`docs/NATIVE_PORT.md` is the detailed reference: build options, every setting
and environment variable, the smoke-test hooks, and the platform notes.
