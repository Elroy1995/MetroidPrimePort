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

Alternatively set `MP_DISC`, keep the image beside the executable, or let the
port ask for it: when no disc is found it opens the platform's file dialog and
remembers the answer as `disc_path` in the settings file. There is no prompt
when the port has no window to show one on, as on a build runner. The disc must
identify as **GM8E01, disc 0, revision 0**; other revisions/regions are
rejected. Nod/Aurora supports additional image
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

### A Wayland session hangs at startup

On a GNOME Wayland session the port can pin a core at 100% and never reach its
first frame. The log stops after `Using surface format BGRA8Unorm`, and no
`MP frame` line ever appears. The cause is outside the port: SDL3 chooses its
Wayland backend when `WAYLAND_DISPLAY` is set, and `SDL_ShowWindow` dispatches
pending Wayland events, one of which is libdecor's client-side decoration
configure. libdecor then re-enters GTK layout from inside that dispatch and
never returns. A backtrace of the hung process shows the loop:

```
hypot → cairo_scaled_font_create → pango_context_get_metrics
      → gtk_widget_get_preferred_height → libdecor_frame_commit
      → decoration_frame_configure → SDL_waylandwindow.c → Wayland_ShowWindow
      → SDL_ShowWindow_REAL → aurora::window::show_window
```

Run with `SDL_VIDEODRIVER=x11` (and `DISPLAY` plus `XAUTHORITY` pointing at the
session's Xwayland) to use the X11 backend instead, which reaches the main loop
normally. The port does this for you: when `SDL_VIDEODRIVER` names no driver and
`DISPLAY` is set, it requests `x11` and says so in the log. Keying off `DISPLAY`
rather than `WAYLAND_DISPLAY` matters — SDL3 reaches for Wayland even with
`WAYLAND_DISPLAY` unset, falling back to the default socket in `XDG_RUNTIME_DIR`.
With no `DISPLAY` there is nothing to fall back to, so the port prints why it is
about to hang rather than leaving only the frozen frame to go on. Neither backend
is a workaround for missing functionality — the only difference is which window
system draws the window.

### Time to first frame

The port prints what startup cost, once a frame has been presented:

```
MP startup: first frame 913 ms after the main loop began, 2349 frames run
```

This is the time from entering the main loop to the first presented frame, which
is where shader compilation, pipeline creation and the first texture uploads
happen. It is **not** time from launching the process: the disc image is read,
mounted and identified before the loop is entered, and a cold shader cache or a
slow disc will dominate that part instead.

Measured on the development machine (RTX 5070 Ti, warm pipeline cache, cutscenes
skipped, `SDL_VIDEODRIVER=x11`): **913 ms, 1180 ms, 1526 ms** over three runs.
The spread is the first-frame work varying with what the driver had cached, so
treat it as roughly a second rather than a precise figure. The existing F1
Performance tab still reports the steady-state render and simulation rates once
running.

### Frame pacing

Measured on Linux with `MP_TRACE_TIMING=1`, sampling the frame log by arrival
time so the wall-clock interval is what is measured rather than the frame's own
cost (`build/frame-pace.sh`):

| | presented | throughput | mean interval | p99 | jitter p99-p50 | late by >1 ms |
|---|---|---|---|---|---|---|
| capped (default) | 60.00 FPS | 1282.8 FPS | 16.667 ms | 16.672 ms | 5.6 us | 0 of 24 |
| uncapped (`F10`) | 137.0 FPS | 137.0 FPS | 7.126 ms | 10.534 ms | 3277 us | 0 of 48 |

Two rates are reported because they answer different questions, and the gap
between them is the useful part:

- **presented** — frames that reached the screen per second of wall time. What a
  player perceives.
- **throughput** — what the machine could produce, with the pacing wait excluded.

At the default cap the two differ by 21x, so stutter here is never the CPU
running out of budget. Uncapped they converge, which is the check that the two
really are measuring different things: once nothing is waiting, they must agree.
Simulation stays at 60 ticks/s either way, because it is fixed-step — uncapped
presentation renders the same simulation more often rather than advancing it
faster.

Under **Xvfb with `SDL_AUDIO_DRIVER=dummy`**, so treat the throughput figure as a
best case. The presented rate and the jitter are the parts that transfer.

### Seeing the front end, and where it stops

With `MP_FAST_BOOT` **off** the real front end runs and waits for input. The
screens captured on the real session, on the AMD adapter, are in
`docs/images/`:

| Screen | File |
|---|---|
| Metroid Prime title | `front-end-title.png` |
| Nintendo publisher logo | `front-end-publisher.png` |
| Dolby Surround Pro Logic II | `front-end-dolby.png` |
| Title with `[ PRESS START ]` | `front-end-press-start.png` |

All three render correctly, so **the reported blank front end does not
reproduce**. Note that `MP_FAST_BOOT=1` never shows any of them: it drives
`Title -> FileSelect -> TransitionToFive()` into a new game, which is why
captures taken with it set are pictures of the *game's* opening, not the front
end.

**Input is discarded when the window is not focused**, which is worth knowing
before concluding that a screen is unresponsive. `CDolphinController::ReadDevices`
(`src/Kyoto/Input/CDolphinController.cpp:105`) zeroes the whole pad status when
`SDL_GetKeyboardFocus()` is null and then preserves the error code, so the
controller still reports *present* while every button is dropped, and the front
end receives input messages containing no buttons. A scripted press launched into
an unfocused window on a live desktop is therefore thrown away before anything
reads it. `MP_SMOKE_SCRIPT` and `MP_SMOKE_FRONTEND` now claim focus the way
`MP_SMOKE_MOUSE` already did; with that fixed the front end reaches the title and
its `[ PRESS START ]` prompt (`front-end-press-start.png`).

The front end then loops between the title and the attract movie. The main menu,
and the Continue option on it, has not been reached: at the few frames per second
this path presents, a single scripted press is a lottery — one during a fade-in
is ignored — and the loop's phase drifts with machine speed, so a press timed
from a fixed frame number does not land. Not a suspected defect, just not done.

Two things to know when reproducing this:

- **The card follows the executable, not `MP_USER_PATH`.** `CARDSetBasePath` is
  given `SDL_GetBasePath()`, so each build directory has its own card. A save
  written by one build is invisible to another.
- **A single press at a guessed frame is not enough.** The title takes ~2000
  frames to fade in on this path, and a press during the fade is silently
  ignored, which is indistinguishable from "the button does not work". Use
  `MP_SMOKE_FRONTEND=<frame>`, which taps repeatedly.

### Audio

A run reports which backend it got, and with what:

```
MP audio: driver pulseaudio
```

or `MP audio: no driver; SDL opened no audio device`. SDL3 picks the first
available backend, so on Linux this is normally `pipewire` or `pulseaudio` — SDL
loads both at runtime, which is why neither appears in `ldd` — and it is the only
place a missing `libpulse` becomes visible at all. Force one with
`SDL_AUDIO_DRIVER`; `SDL_AUDIO_DRIVER=dummy` is what the automated runs use so
they do not fight over a real device.

There is deliberately **no** mixer level or queue-depth report. The mix happens in
vendored MusyX, not in the port, so measuring it there means instrumenting a
vendored snapshot, and a report taken from the port's AI DMA path would describe a
path that is idle in a normal run.

### Choosing a GPU, and why the headless runs all use the NVIDIA one

The loader picks a GPU on its own, which is fine on a desktop and a problem on a
machine with more than one. To pin it, restrict the ICD — no code change is
involved:

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json   # AMD (RADV)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/nvidia_icd.json   # NVIDIA
```

Worth knowing on a multi-GPU machine: pinning a vendor is not the same as pinning
a device. `MESA_VK_DEVICE_SELECT` did not move the choice off the Radeon RX 7900
XTX onto the adjacent Raphael iGPU, so the selector matched a name the loader was
not using. It does not matter for VRAM purposes — either AMD adapter has its own
memory — but "use the integrated GPU" is not reliably expressible through the
environment.

**Headless runs on Xvfb only present with the NVIDIA driver.** Xvfb exposes no
DRI3 extension, and Mesa's Vulkan WSI requires it:

```
MESA: info: vulkan: No DRI3 support detected - required for presentation
```

With the AMD ICD the run selects an adapter, reports its limits, creates the
device, and then dies at surface creation with **0 frames** — so it never gets far
enough to need the textures it was out of memory for. The same Xvfb works with
the NVIDIA driver, which is why every automated run so far has used the discrete
NVIDIA card. Software Vulkan (lavapipe) fails the same way, more abruptly,
segfaulting inside Dawn's surface setup rather than logging.

So on a headless box the GPU is not the thing to change; the display server is.
Free VRAM on the card you are already presenting through, or run against a real X
session or Wayland, where DRI3 exists.

### Running out of device memory

A Vulkan allocation that does not fit is fatal:

```
[fatal] [aurora::gpu] WebGPU error 3: vkAllocateMemory failed with VK_ERROR_OUT_OF_DEVICE_MEMORY
 - While calling [Device].CreateTexture([TextureDescriptor ""GX Static Texture""]).
```

and the process stops. The message names an allocation and nothing else, so the
first reading of it is that the port is holding too much. **It usually is not.**
The port's own process uses about **118 MiB** of device memory while running, a
full eight-world tour does not accumulate allocations, and the failure reproduces
**23 frames into a one-area tour** — immediately after the tour completes and
never during it, which is the opposite of cumulative exhaustion. What actually
caused the failure observed here was an LM Studio model holding **15124 of
16303 MiB**, leaving about 1.1 GB for everything else.

So the first thing to check is what else is on the GPU:

```sh
nvidia-smi --query-compute-apps=pid,used_memory --format=csv
```

Not fixed: the port aborts on a fatal allocation with a message that does not say
what was short, which a player on a busy GPU would see as an unexplained crash.
The honest fix and the smaller-attachment fallback both belong in vendored Aurora,
so they are better decided there than worked around from the port.

### HD texture replacements

`MP_TEXTURES` (default `<executable dir>/textures`) points at a folder of
Aurora-format replacements (`tex1_<w>x<h>_<texhash>[_<tluthash>]_<fmt>.dds` or
`.png`). The folder may hold per-device subfolders — `xbox`, `playstation`,
`switch`, `gamecube`, `standard`, `keyboard` — selected from the input the
player last used (a pad's type; keyboard for a keyboard or mouse, or when no pad
is connected; the Android touch overlay's layout), so in-game button prompts
match what is in the player's hands. Only deliberate input switches it: a key
press, a mouse move or click, a pad button, or a stick or trigger pushed past
half way; the touch overlay's virtual pad does not count. The set is swapped
automatically, and `MP_TEXTURE_DEVICE` forces the name. A folder with no subfolders is used as a
single device-agnostic pack; a folder that has device subfolders but not the one
selected loads nothing rather than mixing packs. `MP_DUMP_TEXTURES=1` writes
every source texture to `<cachePath>/texture_dumps` as DDS, for authoring
replacements.

In-game button prompts are ordinary textures (`CFontImageDef` holds one texture
per glyph), so they can be swapped the same way. To re-author them: dump the
textures from a screen that shows the prompt, find the glyph by its size and
contents, then write a replacement with the same stem into the device folder.
`MP_DUMP_TEXTURES=1` is the supported way to do this and needs no code change:
textures that already have a replacement are not dumped, so the result is
exactly the unclaimed set, and the images can simply be looked at. To see every
texture on the disc instead, `tools/extract_textures.py <disc.iso> <outdir>
--png` writes them all under Aurora's names, with the developers' PAK names
(`LStickN`, `AButtonIn`, `DPadU`...) in `index.tsv` and the textures the game's
text draws inline (`&image=` tags, the HUD hints) in `strg_images.tsv`; that is
how the prompt table was completed. It needs libxxhash, numpy and PIL. The prompt
table in `platform/port_prompts.cpp` is the authority on which textures are
claimed, and every hash in it should be one that appears in a dump from the
screen that shows it - two rows were transcribed into the wrong action and that
is how the pause menu's Exit prompt ended up drawing the C-stick's glyph.
`tools/make_prompt_glyphs.py <dir>` builds a set for every prompt in the table
(xbox, playstation, switch, keyboard) into `<dir>/<device>/`, compositing the CC0 icons
vendored in `tools/prompt_icons/` (Kenney's Input Prompts pack); the keyboard
set labels the keys the port binds by default. The build copies `textures/` next
to the executable, so a built binary picks the replacements up without a manual
copy. Two details matter: the icon must keep the game's inset (the glyph is only
about 22px in the 32x32 buttons and 28px in the 64x32 stick and D-pad art, and a
full-bleed icon reads as a square), and DDS is used
rather than PNG because Aurora's DDS path preserves the texture's alpha.

The button prompts are binding-aware: `platform/port_prompts.cpp` registers, at
runtime, the icon for the input actually bound to each action, so rebinding a
key or mouse button changes the prompt. The per-input icons live in
`<textures>/bindings/` (also generated by `tools/make_prompt_glyphs.py`) and are
served through Aurora's virtual-replacement callback rather than shipped as a
fixed set. Each screen draws its own prompt art, so one action maps to several
game textures. Every button, stick and D-pad prompt texture on the disc is in
the table (A, B, X, Y, Start, L, R, Z; the stick, C-stick and D-pad whole and
per direction), which covers the front end, the pause and map screens and the
HUD hints (the visor hints' D-pad arrows, the beam hints' C-stick, the strafe
and R animations). The sticks follow their axis bindings: a pad shows the stick
that drives the axis (so Southpaw swaps them) or, for an axis driven by buttons,
those buttons; a keyboard shows the key for a direction frame and, for the
whole stick, the WASD, IJKL or arrow cluster when the four keys form one (the
up key otherwise). The stick's diagonal frames take the whole-stick icon. A
keyboard follows its key bindings (the main key, else the second
key); a pad follows its button mapping, so a remapped button or a preset shows
the button it now uses. Letters, digits, punctuation, the named keys, the keypad
(its digits share the main row's art) and all five mouse buttons have icons; a
binding without one leaves the static set in place. A GameCube pad (adapter or
NSO) on its default mapping keeps the game's own art, and only an action moved
to another button gets a GameCube icon, named after the button's default action.
The binding icons register above the static set, so reloading that set never
hides them. The chosen icon is logged when it changes ("prompt A keyboard_x").
`MP_SMOKE_BIND_A=<scancode>` (negative for mouse buttons, -3 is middle) rebinds
the A action once, for checking this without going through the Controls tab; it
needs a `-DMP_ENABLE_SMOKE_DRIVER=ON` build such as `build/smoke-gcc`, and
`MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1 MP_SMOKE_PAUSE=<ticks>` reaches the pause
screen quickly to see the result.

### Platforms

The port is built and tested on Linux and Windows. Its own platform code is
portable - SDL3 and `std::filesystem` throughout - and the CMake keeps the MSVC
linker paths from the template. The Windows build is exercised by
`.github/workflows/windows.yml` on `windows-latest`, which only runs when
started by hand for now (`gh workflow run windows.yml --ref port`): it configures with
clang-cl, builds, runs `ctest -L port`, runs the FIFO regressions, packages a
`dist/` directory with licences, checks that the packaged executable reaches
main and reports the missing disc image, and uploads the result as an artifact.
All of that is green.

Only the things that need a GPU, a window or a real controller are verified on
one platform alone. `wss://` is verified on both: each CI job runs
`port_ws_tests` directly as well as through ctest, so the log shows a completed
TLS handshake against a throwaway CA plus the four rejections (wrong CA,
certificate for another address, system trust store, missing CA file) rather
than only a configure line saying OpenSSL was found.

`platform/glibc_compat.c`, which lowers the glibc the Linux build needs, is
guarded to Linux and takes no part elsewhere. The AppImage and Flatpak packaging
are Linux-only.

Note that `README.md` is inherited from the upstream decompilation project and
describes building that, not this port.

### Distribution

`tools/make_appimage.sh [build-dir] [output-dir]` packages the executable and
its texture replacements as an AppImage, fetching appimagetool on first use.
The disc image is deliberately not included, so the port asks for it with the
platform's file dialog on first launch and remembers the answer as `disc_path`
in the settings file. A path given as an argument or in `MP_DISC` still wins,
then the saved path, then a copy beside the executable.

The AppImage bundles the executable (Aurora, WebGPU/Dawn and SDL3 are linked
statically), the texture replacements, the shared libraries a base desktop may
lack (freetype, libpng, zlib, bzip2 and brotli), and the third-party notices
for the vendored and fetched components in
`usr/share/licenses/metroid-prime-port/`, alongside a
`BUNDLED_LIBRARIES.txt` naming the host libraries it copied. It relies on the
system for glibc, libstdc++, a Vulkan driver, X11 or Wayland, and DBus for the
file dialog.

`docs/RELEASING.md` covers what a release has to carry, what has not been done
yet, and the licensing position.

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

A Flatpak sidesteps the whole question: `tools/make_flatpak.sh` builds one from
`flatpak/io.github.odrannnn.metroidprimeport.yml`, and glibc then comes from the
runtime (24.08) rather than the host, so the floor above does not apply. The GPU
driver still comes from the host, the disc is not bundled, and the sandbox sees
the home directory read-only. It needs flatpak and flatpak-builder and compiles
the whole game, so it is not part of the normal build; change the app id in the
manifest before publishing. This path has not been built here - flatpak is not
installed on the machine it was written on.

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
  aspect-matched in-game HUD frames keep each element's shape but move it away
  from the screen centre, so edge elements (scan panels, energy bar, map) reach
  the true wide corners instead of being pulled inward. Under a perspective
  camera the element is rotated rigidly about the eye rather than slid sideways,
  since sliding turns off-axis elements away from the viewer and shears them;
  the rotation leaves what is seen of the element unchanged and the angle is
  derived from the aspect ratio, so it holds at any aspect rather than only
  16:9. Menus, the credits and other non-aspect-matched frames are unaffected.
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
- `MP_TURBO[=<ticks>]`: lockstep for automated runs. Every frame runs exactly
  `<ticks>` fixed ticks (default 1, at most 16) with no frame limiter and no
  vsync, so a run goes as fast as the machine renders it; game time per tick
  stays exact. Under Xvfb presentation caps near 100 fps, so extra ticks per
  frame are what give the speedup: 3000 ticks took 50 s at real time, 30 s at
  `MP_TURBO=1`, 7 s at `4` and 3.3 s at `8`. Audio and streamed music do not
  keep up. Not saved to the settings file. On exit the port prints
  `MP run: <frames> frames in <s> s`.
- `MP_TOUCH_UI=1`: use the touch layout for the debug overlay on desktop (always
  on for Android): a full-screen window inside the safe area, with a page list
  instead of tabs, larger hit targets, drag-to-scroll with fling and a Close
  button.
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

`MP_SMOKE_ELEVATOR=<ticks>` rides the elevator most recently loaded in the current world
once gameplay has run for `<ticks>` ticks. It sends the elevator's `Play` and
`SetToZero` messages, like the ride trigger does, and reports
`[elevator-smoke] passed: world <id> area <n>` once the destination world is
playable. Combine it with `MP_SMOKE_WORLD=83F6FF6F` (Chozo Ruins, whose spawn
area loads the Tallon elevator). It reproduced the elevator crash:
`CWorldTransManager::WaitForModelsAndTextures` bounced model buffers through ARAM,
which over-read them and freed host `new[]` memory into the game heap. The port
now skips that model pass.

`MP_SMOKE_VISOR=1` grants and switches to the thermal visor (`MP_SMOKE_VISOR=xray`:
the X-ray visor) after gameplay starts and reports `[visor-smoke] passed` once it has stayed up. It reproduces
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
With `MP_SMOKE_WORLD` set, the walk waits until the world jump has finished, so
it starts in the destination room. For example, this reaches the Parasite Queen
fight (Frigate Orpheon, Reactor Core) in about 15 s:
`MP_TURBO=8 MP_SMOKE_WORLD=158EFE17 MP_SMOKE_WORLD_AREA=87452DC1 MP_SMOKE_WALK=3000 MP_SMOKE_FRAMES=1500`
together with `MP_FAST_BOOT=1 MP_SKIP_CUTSCENES=1`.

`MP_SMOKE_STICK=1` holds the right stick and reports the aim yaw change, to
verify twin-stick aiming (run with `MP_TWIN_STICK=1`).

`MP_SMOKE_PAUSE=<ticks>` enters the pause screen after gameplay starts;
`MP_SMOKE_MAP=<ticks>` presses Z that many ticks into gameplay to open the map
screen (Z opens the map from gameplay; in the pause screen it does nothing).
Combine with `MP_SMOKE_SHOT` to capture them.
`MP_SMOKE_FRONTEND=<frame>` taps Start every 300 frames from that frame, so the
front-end screens (title, save dialogs, main menu) are reached without a player;
Start alone leaves dialogs on screen rather than dismissing them.
`MP_SMOKE_CONTINUE=1` walks the title and file select to Continue on slot 1. It
does not combine with `MP_FAST_BOOT`: the walker only acts at file select, and
fast boot is what leaves file select, so with both set the run quietly becomes a
new game instead. This has always been so; set only `MP_SMOKE_CONTINUE` to test
the Continue path.

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
