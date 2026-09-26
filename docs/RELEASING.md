# Release readiness

What a build of this port contains, what has to travel with it, and what is
still missing before it can be handed to someone else. Every claim here is
checked against the tree; where something is not true yet, it says so.

## What the port is

A native recompilation of **Metroid Prime** (`GM8E01`, USA v1.00) plus a
platform layer. The game code is statically recompiled from the retail
executable, so **no game assets, no disc image, and no extracted data may be
packaged with a build**. The build takes the disc image as a runtime argument
and nothing else: `tools/make_appimage.sh` says so in its own help text, and
the port asks for the image on first launch when it cannot find one.

## Licensing

| Component | Licence | Where it is |
|---|---|---|
| Aurora (windowing, WebGPU/Dawn plumbing) | MIT | `extern/aurora/LICENSE` |
| MusyX (audio mixer) | MIT | `extern/musyx/LICENSE` |
| SDL3 | zlib | fetched at configure time, `build/*/_deps/sdl-src/LICENSE.txt` |
| Dear ImGui | MIT | fetched, `imgui-src/LICENSE` |
| fmt | MIT | fetched, `fmt-src/LICENSE` |
| zstd | BSD | fetched, `zstd-src/LICENSE` |
| Button prompt icons | Kenney CC0 | `tools/prompt_icons/`, built into `textures/` |

**The repository has no top-level licence grant.** That is the one thing that
must be settled before this is distributed: a build links the MIT and zlib
components above, whose notices have to accompany it, and the decompiled game
source is a separate question this document does not answer. Anyone packaging
a release should treat that as an open legal question and get it answered,
rather than inferring permission from the absence of a `LICENSE` file.

### Notices must travel with a build

- **Windows** already does this: `windows.yml` copies Aurora's and MusyX's
  licences plus every `LICENSE*`/`COPYING*`/`NOTICE*` from the fetched packages
  into `dist/licenses/`, preserving the dependency path, and puts
  `docs/NATIVE_PORT.md` in as the `README`.
- **The AppImage and the APK now do.** `tools/make_appimage.sh` collects the
  vendored and fetched notices into `usr/share/licenses/metroid-prime-port/`
  and records which shared libraries it bundled in `BUNDLED_LIBRARIES.txt`, so
  a reader can tell what came from where. The APK's `syncLicenseNotices` task
  gathers the same set into `assets/`, verified present in a built package:
  `aurora.txt`, `musyx.txt`, `sdl-src.txt`, `imgui-src.txt`, `fmt-src.txt` and
  `zstd-src.txt`. The notices for the libraries an AppImage copies from the
  build host are still named rather than reproduced — see below.
- **The Flatpak still does not.** Nothing in `tools/make_apppak.sh`'s manifest
  collects notices, and the Flatpak has never been built here.
- **The shared libraries an AppImage copies are named, not reproduced.** They
  come off the build host rather than out of the tree, so their terms cannot be
  collected automatically; `BUNDLED_LIBRARIES.txt` says which were bundled and a
  real release should ship their licence texts too.

## Signing identity

**Android signs release builds with the debug key.** `android/app/build.gradle`
sets `signingConfig signingConfigs.debug` for the `release` variant, which is
what makes `:app:assembleRelease` installable without extra setup — and is
exactly what a real release must not do. The debug key is not an identity: it
is shared, it is not private, and it offers no assurance about who built the
package. A real release needs its own keystore, kept out of the repository,
supplied at build time.

The port targets `versionName "0.1.0"` and `versionCode 1`.

## Per-platform packaging

| Platform | Builds from | Produces | State |
|---|---|---|---|
| Linux | `cmake -S . -B build-gcc` | executable | works; tests green |
| Linux | `tools/make_appimage.sh` | AppImage | builds; **notices missing** |
| Linux | `tools/make_flatpak.sh` | Flatpak | **never built here** — no `flatpak-builder` on the development machine; the app id in the manifest must be changed before publishing |
| Windows | `.github/workflows/windows.yml` | zipped `dist/` | green in CI, artifact uploaded, packaged startup checked |
| Android | `tools/android_apk.sh :app:assembleRelease` | APK | builds a signed debug-key APK; **on-device behaviour unverified** |

The Linux binary is the only one with a test suite attached: 14 `port`-labelled
ctest targets, all run by both CI jobs.

## Runtime dependencies

The AppImage deliberately does **not** bundle glibc, so the build is only as
portable as its build host. `platform/glibc_compat.c` lowers that floor to
**glibc 2.39** (Ubuntu 24.04, the current LTS) by defining the newer symbol
versions in terms of the older ones. Above that, `pidfd_spawnp`/`pidfd_getpid`
used by nod would need an older nod build or an older base.

A Vulkan driver, X11 or Wayland, and DBus for the file dialog come from the
host. A session that reaches `show_window` over SDL's Wayland backend will hang
under GNOME unless the port's own X11 preference applies — see
`docs/NATIVE_PORT.md`.

## What still blocks a real release

1. **No top-level licence grant**, and no answer for the decompiled game source.
2. **Notices still missing from the Flatpak**, and the AppImage only *names*
   the shared libraries it copies rather than reproducing their terms. Windows,
   AppImage and APK all collect the rest.
3. **Android release signing** is the debug key.
4. **Android on-device behaviour is unverified** — boot, render, save, play a
   seed and connect all need a device.
5. **`wss://` does not work on Android**: the NDK has no OpenSSL, so the
   build has no `MP_HAVE_OPENSSL` and a `wss://` server is refused rather than
   downgraded. A JNI `SSLSocket` backend or a vendored TLS library is what that
   needs.
6. **The Flatpak path has never been built**, and its app id is a placeholder.
