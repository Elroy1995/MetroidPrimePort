# Native Metroid Prime port

This branch compiles the decompiled game and Kyoto engine as native C++20.
Aurora provides the GameCube SDK compatibility layer, SDL3, and WebGPU/Dawn;
MusyX provides the software audio engine. It does not link DolRecomp or the
ModernGekko runtime from the separate recompilation experiment.

## Build

Requirements: CMake 3.25+, Ninja, a C++20 compiler/library, and a C compiler with
C23 support for MusyX. Tested locally with GCC 15 and Clang 19 on x86-64 Linux;
CI is configured for GCC 14, Clang 18, and Windows clang-cl. Windows requires LLVM and an
MSVC developer environment/Windows SDK. Older compiler versions are unverified.

Aurora and MusyX are vendored snapshots; a normal clone is sufficient. Aurora
fetches pinned transitive dependencies on the first configure. On Linux install
SDL build prerequisites (X11/Wayland, ALSA/PulseAudio, EGL/OpenGL, FreeType, PNG);
see `.github/workflows/native-linux.yml` for the Ubuntu package list and
`extern/aurora/docs/building.md` for dependency-provider options.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native -j 4
ctest --test-dir build/native -L port --output-on-failure
```

For Windows, run from an MSVC developer shell and add
`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl` to configure.
Plain MSVC `cl.exe`, MinGW, macOS, and ARM builds are not currently validated.

No disc or generated game-asset headers are needed to compile. The two embedded
default-font resources are read from the mounted retail DOL at runtime, using
the addresses in `config/GM8E01_00/symbols.txt`. Native builds do not depend on
`build/GM8E01_00/include` or distribute extracted font data.

### Linking and packaging

`mp_game` is an OBJECT library: every source in the game manifest is deliberately
linked once, so COFF archive extraction does not decide which game/shim-dependent
translation units are present. Duplicate-definition errors remain enabled;
neither whole-archive nor `/FORCE:MULTIPLE` is used. The Aurora core/GX static
library cycle uses `LINK_GROUP:RESCAN` on supported ELF linkers; COFF resolves
those archive dependencies normally.

Keep `initial_pipeline_cache.db` beside the executable. Windows also needs the
runtime DLLs copied there by CMake, including Dawn's dynamically loaded DXC
libraries when provided. Use the CI `dist` package rather than copying just the
EXE. The workflow runs regression tests and checks the packaged no-disc startup
path. Preserve the accompanying dependency licenses/notices.

## Run

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

Alternatively set `MP_DISC`. The disc must identify as **GM8E01, disc 0, revision
0**; other revisions/regions are rejected. Nod/Aurora supports additional image
containers, but the same retail content is required.

Aurora selects user/cache directories through its SDL platform paths and logs
them at initialization. `MP_USER_PATH` and `MP_CACHE_PATH` override these with
explicit directories; use separate directories for automated testing so runs do
not share normal saves/settings. Screenshots are written to `screenshots/` in
the working directory. `MP_TEXTURES` points to an optional replacement pack.

### Controls and settings

- Keyboard defaults: WASD / IJKL for sticks, X/Z/C/V for A/B/X/Y, Return for
  Start, arrows for D-pad, Q/E for L/R, and F for Z. Existing mappings take
  precedence. SDL controllers are supported.
- F1: debug overlay. F10: 60 FPS cap/unlimited presentation. F12: screenshot.
- `MP_ASPECT=4:3|16:9|window`; the legacy `MP_WIDESCREEN` selects 16:9.
- `MP_MOUSE_AIM=1`, `MP_MOUSE_SENS=0.0035`: relative mouse aim. Motion is ignored
  while the overlay is visible or relative capture/focus is absent. Mouse mode
  uses immediate yaw/pitch with an approximately ±87° pitch range. Up moves aim
  up by default; `MP_MOUSE_INVERT_X=1` and `MP_MOUSE_INVERT_Y=1` invert either axis.
  SDL and the compositor own pointer locking; capture is released outside
  playable first person (menus, cinematics, morph ball, and scripted input locks).
- In mouse mode, **left-click fires / holds a charge / releases a charged shot**,
  **right-click holds lock-on**, and **middle-click fires missiles**. These feed
  the normal PAD/gun input path, preserving charge timing and weapon cooldowns.
  Existing keyboard/controller weapon bindings are also available; saved mapping
  files are not rewritten. `MP_DISABLE_MOUSE_BUTTONS=1` opts out of these aliases.
- Mouse mode requests the GC aiming crosshair without holding R or entering the
  console's movement-restricting free-look mode. `MP_DISABLE_MOUSE_CROSSHAIR=1`
  opts out. The Input tab exposes inversion, weapon-button and crosshair toggles.
  Lock-on owns the camera while held; releasing it resumes at the actual locked
  direction rather than at accumulated mouse angles. Jump/fall auto-pitch is
  bypassed during free mouse aim. Capture/UI transitions cancel held charges and
  require mouse-button release before another mouse shot can begin.
- `MP_DISABLE_AI_AUDIO=1`: start streamed AI audio muted. It can subsequently be
  enabled from the overlay. MusyX mute is independent.
- `MP_FAST_BOOT=1`, `MP_SKIP_CUTSCENES=1`, `MP_CUTSCENE_SPEED=8`,
  `MP_SHOW_DEBUG_UI=1`: development controls. Presence flags are enabled by
  being set; unset them to disable them. Cutscene speed is restricted to 1–32.
- `MP_VALIDATE_SAMPLES=1`: log MusyX sample-directory validation.

The simulation uses a 60 Hz accumulator independently of the presentation cap.
Ordinary slow frames catch up; pauses/debugger stalls are capped to 250 ms of
simulation work per iteration. Audio runs on wall-clock/device consumption.
Hidden windows continue pumping events and main-thread audio without recording
rendered frames. Restart-to-menu rebuilds the game architecture instead of
attempting a console reboot.

In uncapped presentation, free mouse aim uses the current simulation orientation
so the visible reticle does not lag the shot direction; camera translation still
interpolates. The held cannon/arm and muzzle effects render against the matching
simulation camera, then restore the world view before world-space effects. This
keeps the viewmodel stable instead of mixing an interpolated view with a cached
60 Hz gun transform. Weapon animation and projectile simulation remain 60 Hz.

## Ownership and threading rules

- Use `rstl::auto_ptr<T[]>` / `single_ptr<T[]>` for host arrays and scalar owners
  for host objects. `rstl::game_memory<T>` selects `CMemory::Free` for game-heap
  buffers. `rs_new` is ordinary host `new` in the native build.
- `rstl::auto_ptr` still transfers ownership on copy and `release()` retains a
  non-owning view; do not treat it like `std::unique_ptr`.
- Reference-counted and resource owners capture a deleter where the type is
  complete; their release sites can safely live in forward-declaration-only
  translation units. This avoids inconsistent template destructor definitions.
- MusyX pins resources for its bounded pushed-group stack. Group pop holds the
  IRQ mutex while detaching software voices and unregistering data, so samples
  can be freed afterwards. Audio output is joined before DSP state is destroyed.
- DVD callbacks can run on the DVD worker. ARAM file queue/state/lifetime changes
  share one recursive mutex. ARQ completions are pumped on the main thread.
- FIFO draw-sync tokens protect CPU-side consumption of referenced arrays.
  Synthetic VI retraces are bookkeeping callbacks, not WebGPU completion fences.
- If `BeginScene()` returns false, do not draw or call `EndScene()`. Delayed
  render-resource retirement advances only after a successful frame.

Saves write the retail big-endian CRC word. Reading also accepts the early
native port's little-endian CRC representation for compatibility. Bit fields
are serialized MSB-first on both host byte orders.

## Validation

CTest's `port` label covers array/game-heap ownership, golden save bit fields,
truncated-stream errors, pathfinder bitset bounds, AI enable/callback teardown,
the MusyX pointer-sized DMA API, and DOL section mapping/bounds. It does not
require a disc or GPU.
`port_mouse_tests` checks direction/inversion, pitch limits, yaw normalization,
lock and non-first-person handoffs, and held-button/capture edge behavior.
The CARD regression creates its own `card-test-data` directory under the build
tree, tests null callbacks and file operations, and closes/reopens the backing
store before comparing the saved bytes. It never uses the normal game profile.
Real-disc smoke runs also created a native `MetroidPrime A.gci` in an isolated
profile and reopened that profile in a subsequent process.

For a real-disc lifecycle run, configure with `-DMP_ENABLE_SMOKE_DRIVER=ON`:

```sh
MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_DISABLE_AI_AUDIO=1 \
MP_SMOKE_FRAMES=2400 MP_SMOKE_LIFECYCLE=1 SDL_AUDIO_DRIVER=dummy \
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

This opt-in driver hides/restores the window, mutes/unmutes both audio paths,
toggles presentation pacing, restarts to the menu, and exits normally. Omit
`MP_SMOKE_LIFECYCLE` for a bounded ordinary run. Production builds omit the
driver unless enabled at configure time.

`MP_SMOKE_MOUSE=1` enables an additional deterministic real-disc scenario in that
build. Run it with `MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_FRAMES=1800` and an
isolated `MP_USER_PATH`/`MP_CACHE_PATH`. It injects mouse input without grabbing
the real pointer, verifies immediate camera aim, live power/missile projectiles,
charged release, lock/release, jump and morph-ball handoffs, UI charge cancellation,
crosshair state, and cannon/view orientation while uncapped. Scripted cinematics
pause the test sequence; the frame limit is a minimum until the scenario finishes.
Success is reported as `[mouse-smoke] passed` followed by a clean exit.

For AddressSanitizer, use a separate Clang build with
`-DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"` and the same
`CMAKE_CXX_FLAGS`. Keep allocation/deallocation mismatch checks enabled.
`ASAN_OPTIONS=detect_leaks=0` can isolate memory-safety/lifetime checks from
process-global caches; such a run does not establish leak freedom.

With `-DAURORA_ENABLE_TESTS=ON`, build and run `gx_fifo_tests` under
`build/native/extern/aurora/tests/`. This GPU-free suite includes concurrent
producer/worker append, buffer growth, and draw-sync ordering. All 205 FIFO/GX
tests passed in the local ThreadSanitizer build. Full-application TSan validation
is currently blocked by reports in uninstrumented system GLib/libdbus and the
prebuilt Rust nod preloader before game execution; no whole-game TSan-clean
claim is made. The standalone FIFO suite avoids those dependencies.

Manual validation still matters: door/room traversal, map/pause transitions,
retail/Dolphin save import/export, interrupted saves, controller hot-plugging,
audio pitch/tempo by ear, and Windows packaged gameplay on a clean machine.
The automated lifecycle driver does not claim full-game coverage.
