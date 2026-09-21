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

`metroid_prime_port --version` prints the source revision without initializing
graphics. The same revision appears in the launch log and F1 Performance tab.
Include it when reporting a copied build from another machine; a `-dirty` suffix
means the executable was built with uncommitted source changes.

Aurora selects user/cache directories through its SDL platform paths and logs
them at initialization. `MP_USER_PATH` and `MP_CACHE_PATH` override these with
explicit directories; use separate directories for automated testing so runs do
not share normal saves/settings. Screenshots are written to `screenshots/` in
the working directory.

A copied build is self-contained by default: the memory card is written to
`<executable dir>/<region>/Card A`, the disc image is auto-detected next to the
executable (or one level below it), and texture replacements are loaded from
`<executable dir>/textures` when present. `MP_DISC` and `MP_TEXTURES` still
override these.

### HD texture replacements

`MP_TEXTURES` (default `<executable dir>/textures`) points at a folder of
Aurora-format replacements (`tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>.dds` or
`.png`). The folder may hold per-device subfolders — `xbox`, `playstation`,
`switch`, `gamecube`, `standard`, `keyboard` — selected from the connected
controller (keyboard when no pad is connected) so in-game button prompts match
the pad in use. The set is swapped automatically when the active device changes,
and `MP_TEXTURE_DEVICE` forces the name. A folder with no subfolders is used as a
single device-agnostic pack; a folder that has device subfolders but not the one
selected loads nothing rather than mixing packs. `MP_DUMP_TEXTURES=1` writes
every source texture to `<cachePath>/texture_dumps` as DDS, for authoring
replacements.

In-game button prompts are ordinary textures (`CFontImageDef` holds one texture
per glyph), so they can be swapped the same way. To re-author them: dump the
textures from a screen that shows the prompt, find the glyph by its size and
contents, then write a replacement with the same stem into the device folder.
`tools/make_prompt_glyphs.py <dir>` builds a set for the A and B prompts (xbox,
playstation, switch, keyboard) into `<dir>/<device>/`, compositing the CC0 icons
vendored in `tools/prompt_icons/` (Kenney's Input Prompts pack); the keyboard
set labels the keys the port binds by default. The build copies `textures/` next
to the executable, so a built binary picks the replacements up without a manual
copy. Two details matter: the icon must keep the game's inset (the glyph is only
about 22px on screen, and a full-bleed icon reads as a square), and DDS is used
rather than PNG because Aurora's DDS path preserves the texture's alpha.

The A and B prompts are binding-aware: `platform/port_prompts.cpp` registers, at
runtime, the icon for the input actually bound to each action, so rebinding a
key or mouse button changes the prompt. The per-input icons live in
`<textures>/bindings/` (also generated by `tools/make_prompt_glyphs.py`) and are
served through Aurora's virtual-replacement callback rather than shipped as a
fixed set. Each screen draws its own prompt art, so one action maps to several
game textures; the front end, the pause/inventory screen and the map screen are
covered, for the A, B, L, R, Z and stick prompts. The stick prompts (the map
screen's Move, the pause screen's Zoom) are axes rather than buttons, so they do
not follow a binding; they get the device's own stick, or a direction-key icon
on keyboard. The HUD prompts are not covered yet and keep the static icon. A
device set only carries the actions it has art for, so the pad sets have no Z. Only the keyboard/mouse set follows bindings;
with a pad the static per-device icons are used, since they already match the
pad's own labels, and a remapped pad button is not reflected. The chosen icon is
logged when it changes ("prompt A keyboard_x"). `MP_SMOKE_BIND_A=<scancode>` rebinds the A action once,
for checking this without going through the Controls tab, and
`MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_PAUSE=<ticks>` reaches the pause
screen quickly to see the result.

### Distribution

`tools/make_appimage.sh [build-dir] [output-dir]` packages the executable and
its texture replacements as an AppImage, fetching appimagetool on first use.
The disc image is deliberately not included, so the port asks for it with the
platform's file dialog on first launch and remembers the answer as `disc_path`
in the settings file. A path given as an argument or in `MP_DISC` still wins,
then the saved path, then a copy beside the executable.

The AppImage bundles the executable (Aurora, WebGPU/Dawn and SDL3 are linked
statically), the texture replacements, and the shared libraries a base desktop
may lack: freetype, libpng, zlib, bzip2 and brotli. It relies on the system for
glibc, libstdc++, a Vulkan driver, X11 or Wayland, and DBus for the file dialog.

glibc is deliberately not bundled, so the build is only as portable as the
machine it was built on. `platform/glibc_compat.c` lowers that floor: recent
glibc gives the float math functions and the C23 strtol/scanf family new symbol
versions, which would otherwise pin the binary to the build host's glibc
(2.43 here). Defining those names in terms of the long-standing
double-precision and pre-C23 functions brings the requirement down to
**glibc 2.39** (Ubuntu 24.04, the current LTS).

What is left above that is `pidfd_spawnp`/`pidfd_getpid`, used by nod - the
prebuilt Rust library behind Aurora's disc access - which would need either an
older nod build or a build on an older base to remove.

The AppImage embeds the statically linked type-2 runtime, so libfuse2 is not
needed on the target system; check it with `--appimage-version`. Where FUSE
itself is unavailable, such as in a container, run it with
`APPIMAGE_EXTRACT_AND_RUN=1` (or `--appimage-extract-and-run`), which unpacks to
a temporary directory instead of mounting.

### Controls and settings

- Keyboard defaults: WASD / IJKL for sticks, X/Z/C/V for A/B/X/Y, Return for
  Start, arrows for D-pad, Q/E for L/R, and F for Z. Existing mappings take
  precedence. SDL controllers are supported.
- F1: debug overlay. F10: 60 FPS cap/unlimited presentation. F12: screenshot.
- Settings changed in the F1 overlay (aspect, vsync, render scale, frame limit,
  cutscene options, mouse aim/inversion/sensitivity, audio mutes) are saved to
  `port_settings.ini` in the user directory (`MP_USER_PATH`, else Aurora's SDL
  preference path) and restored on the next launch. The Session tab shows the
  path and has a **Save settings now** button. Environment variables still
  override the file for that run, and are written back into it if any setting is
  changed during that run.
- `MP_ASPECT=4:3|16:9|window`; the legacy `MP_WIDESCREEN` selects 16:9.
- `MP_TWIN_STICK=1` (Input tab, persisted as `twin_stick`): twin-stick aiming. The
  right stick feeds the first-person aim through the same path as the mouse (so
  the same sensitivity/invert apply, tuned by `stick_aim_rate`, default 900 px/s)
  and is consumed, so it no longer drives the game's free-look. Fire stays on
  whatever is bound to A; remap it in the Controls tab.
- The overlay's **Controls** tab rebinds pad 1: click Bind, then press the input.
  "Keyboard & mouse" assigns a key or mouse button to each pad button and stick
  axis; "Controller" assigns a physical controller button or axis. Bindings are
  saved by Aurora next to the other controller data, with buttons to clear the
  keyboard bindings and restore the controller defaults.
- `MP_HUD_WIDE=1` (Render tab, persisted as `hud_wide`): widescreen HUD. The
  aspect-matched in-game HUD frames keep each element's shape but spread its
  position about the screen centre, so edge elements (scan panels, energy bar,
  map) reach the true wide corners instead of being pulled inward. Menus, the
  credits and other non-aspect-matched frames are unaffected.
- The in-game pause and map screens (`FRME_PauseScreen`, `FRME_PauseScreenInstructions`,
  `FRME_MapScreen`) are aspect-matched like the HUD, so they keep their
  proportions and spread across a wide viewport instead of stretching.
- The front end (`FRME_FrontEndPL` title/menu, `FRME_NewFileSelect`, the GBA
  screens) is aspect-matched too, so the title screen is pillarboxed rather than
  stretched.
- The mouse cursor is hidden while the game has focus and is shown only over the
  F1 overlay. The overlay is also openable and navigable with a controller: the
  Start+Back chord toggles it (Start is suppressed for the game while Back is
  held, so it does not also pause), the D-pad or left stick moves, A activates, B
  cancels and the shoulder buttons switch tabs (ImGui gamepad navigation).
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
- Outside lock-on, A/D (the left-stick lateral axis) strafe in mouse mode rather
  than applying the console's turning torque. Movement uses the current mouse
  heading and the game's acceleration, friction, surface restraints and collision
  handling. Diagonal input/speed is bounded. Lock-on keeps its native orbit/dash
  behavior; closing F1 does not require reacquiring a lock to strafe.
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
- `MP_SIM_RATE=<hz>`: experimental simulation tick rate (30–480, default 60).
  60 is console-accurate; higher values step the game logic at the display rate
  instead of interpolating the camera. `MP_SIM_ADAPTIVE=1` instead takes one
  step per frame with `dt` = the measured frame time (clamped 30–480 Hz), so a
  variable frame rate is matched exactly. Both are also settable from the F1
  Performance tab and persisted. See `docs/HIGH_FPS_AUDIT.md` for what still
  assumes 60 Hz.

The simulation uses a fixed-step accumulator (60 Hz by default) independently of
the presentation cap. Ordinary slow frames catch up; pauses/debugger stalls are
capped to 250 ms of simulation work per iteration. Audio runs on
wall-clock/device consumption.
Fractional simulation time is retained through frame jitter. The capped scheduler
can borrow at most 0.25 ms near a tick boundary and carries that debt forward, so
it does not alternate zero/two ticks merely due to microsecond sleep jitter.
The Performance tab distinguishes the **60 FPS target** from measured render FPS
and simulation ticks/second. `MP_TRACE_TIMING=1` logs both rates once per second.
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
lock and non-first-person handoffs, movement bounds and held-button/capture edge
behavior. `port_timing_tests` covers fractional-time carry and capped jitter;
`port_audio_math_tests` covers ADPCM partial loops, PCM8 and wide Q15 mixing.
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
The sequence also verifies free strafing on both sides of opening/closing F1.

`MP_SMOKE_AREA_RELOAD=1` exercises three real geometry eviction/ARAM restoration
cycles. It reproduced the material-flags crash seen when opening a door before
the one-time native surface-header conversion fix. It can be combined with the
mouse scenario and reports `[area-smoke] passed`.

`MP_SMOKE_WORLD=<hex MLVL id>`, or `MP_SMOKE_WORLD=auto` to pick the first world
other than the current one, jumps to another world through the same restart path
the in-game world teleporters use. It waits for gameplay, requests the jump, and
reports `[world-smoke] passed: world <id> area <n>` once a freshly constructed
world is running. The F1 debug overlay's Debug tab lists every world by its
front-end name and jumps to it on click.

`MP_SMOKE_VISOR=1` grants and switches to the thermal visor after gameplay
starts and reports `[visor-smoke] passed` once it has stayed up. It reproduces
the FIFO-worker crash where the game binds a texture whose source pointer is an
unmapped value, which the content hash then dereferences. The thermal cold blend
was passing a deliberately fake random address as its noise texture (the console
reads raw memory for noise); the port now fills a real scratch noise buffer.
Aurora also skips any texture whose source page is not mapped instead of hashing
it; set `MP_LOG_TEX_INVALID=1` to log each rejected texture's pointer, format,
size and object id.

`MP_SMOKE_WALK=<ticks>` holds the stick fully forward after gameplay starts and
reports `[walk-smoke] passed: ticks=... dist=... maxFlatSpeed=... speed=.../s`.
Run it with the same real duration at two simulation rates (for example 60 ticks
at 60 Hz and 120 ticks at 120 Hz) to confirm ground movement stays real-time.

`MP_SMOKE_STICK=1` holds the right stick and reports the aim yaw change, to
verify twin-stick aiming (run with `MP_TWIN_STICK=1`).

`MP_SMOKE_PAUSE=<ticks>` enters the pause screen after gameplay starts;
`MP_SMOKE_MAP=<ticks>` presses Z that many ticks into gameplay to open the map
screen (Z opens the map from gameplay; in the pause screen it does nothing).
Combine with `MP_SMOKE_SHOT` to capture them.
`MP_SMOKE_FRONTEND=<frame>` taps Start every 300 frames from that frame, so the
front-end screens (title, save dialogs, main menu) are reached without a player;
Start alone leaves dialogs on screen rather than dismissing them.

For audio reports, `MP_AUDIO_STATS=1` logs MusyX's generated samples/second, queued
audio, peak output and clipping. Nominal output is 32,000 stereo frames/second;
short windows vary with the device's buffering. Static ADPCM loops must wrap at
`loop + loopLength` and restart from the exact loop nibble/history. Streaming
ADPCM retains its predictor history across ring-buffer wraps. These rules are
now distinct; neither is tied to the renderer's frame count.

For stuck or unexpected sounds, `MP_LOG_VOICES=1` logs the active MusyX voices
(sample id, source pointer, length/loop, compression, pitch, volumes and the
listener heading) about three times a second, and `MP_MUTE_SMP=65535,93` silences
voices by sample id so a persistent one can be identified by ear. Streamed
voices report sample id 65535. `MP_LOG_3D=1` logs any 3D emitter whose Doppler
factor is not 1. The overlay's **Voices** tab lists the live voices (loudest
first) with a per-sample mute checkbox and an "Unmute all" button; the muted ids
are saved to `voices_muted` in the settings file.

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
