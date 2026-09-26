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

**The repository's own work is MIT, and the game code is not.** `LICENSE` grants
MIT over the platform layer, the build and packaging scripts, the tests, `tools/`
and `docs/`. `NOTICE` states what that deliberately does not cover: `src/` and
`include/` are a recompilation of the retail game, no permission to redistribute
them is asserted anywhere in this tree, and the grant stops at the repository's
own code rather than sweeping those directories in.

That is a grant over the port, not an answer about the game. Anyone packaging a
release still has to settle the second question — what may be done with a
working copy of decompiled game code — and only the copyright holder can answer
it. The licence here is not evidence that they may.

### Where saves live, and the per-build-directory card

The memory card is placed with `CARDSetBasePath(SDL_GetBasePath())`, so **the card
belongs to the directory the executable is in** — not to `MP_USER_PATH`, which
controls only the dawn cache and the game profile. On desktop that is next to the
binary; on Android it is the app's storage, because the APK is read-only.

The practical consequence is that every build directory has its own card, so a
save written by one build is invisible to another. That is worth knowing when
comparing runs, and worth saying in a support answer, because "my save vanished"
is otherwise inexplicable. It is a deliberate choice — a copied build stays
self-contained — and it is why `MP_USER_PATH` cannot be used to move saves.

### Notices must travel with a build

- **All four do.** Each ships the port's own `LICENSE` and `NOTICE` alongside
  the third-party terms, because a grant nobody can read inside the package is
  not much of a grant.
- **Windows**: `windows.yml` copies the port's grant and notice, Aurora's and
  MusyX's licences, and every `LICENSE*`/`COPYING*`/`NOTICE*` from the fetched
  packages into `dist/licenses/`, preserving the dependency path, and puts
  `docs/NATIVE_PORT.md` in as the `README`.
- **The AppImage and the APK**: `tools/make_appimage.sh` collects them into
  `usr/share/licenses/metroid-prime-port/` and records which shared libraries it
  bundled in `BUNDLED_LIBRARIES.txt`, so a reader can tell what came from where.
  The APK's `syncLicenseNotices` task gathers the same set into `assets/`,
  verified present in a built package: `port-license.txt`, `port-notice.txt`,
  `aurora.txt`, `musyx.txt`, `sdl-src.txt`, `imgui-src.txt`, `fmt-src.txt` and
  `zstd-src.txt`. The notices for the libraries an AppImage copies from the
  build host are still named rather than reproduced — see below.
- **The Flatpak** collects them in the manifest's `post-install`: the two
  vendored snapshots out of the tree, and the four fetched packages by glob,
  because the fetched ones only exist once `cmake-ninja` has run. It has still
  never been built here — see below.
- **The shared libraries an AppImage copies are named, not reproduced.** They
  come off the build host rather than out of the tree, so their terms cannot be
  collected automatically; `BUNDLED_LIBRARIES.txt` says which were bundled and a
  real release should ship their licence texts too.

## Signing identity

**Android release signing is this project's own key.** `tools/make_android_keystore.sh`
creates it and writes `android/keystore.properties`; the file and the `.jks` are
both untracked, and `.gitignore` says so. The build refuses to sign a release
with the shared debug key unless that is asked for explicitly, because the debug
key is not an identity: it is public, it is the same on every machine, and it
says nothing about who built the package.

The refusal matters more than it looks. Android treats a signature change as a
different app, so a debug-signed release has to be **uninstalled** before a
properly signed one will install. Finding that out after shipping is worse than
being told at build time.

Where the four values come from: `android/keystore.properties`, then
`MP_APK_KEYSTORE` / `MP_APK_KEYSTORE_PASSWORD` / `MP_APK_KEY_ALIAS` /
`MP_APK_KEY_PASSWORD` — so a CI secret and a local file take the same path. All
four are required. A partial set is an error naming which are missing, and a
`storeFile` that is not there is an error naming the path, rather than a silent
fall back to the debug key.

`tools/android_apk.sh` opts in on the caller's behalf **only** when no key is
configured, and says so on stderr, because a build that is merely going to be
sideloaded should not need a key to exist first. `--strict-signing` turns that
into a refusal. The rule itself is Gradle's, not the script's: calling Gradle
directly with no key stops the build.

Verified on a release build: the package's signer is
`CN=Metroid Prime Port, OU=Port, O=Metroid Prime Port` (SHA-256 `d8814c79…`),
not the debug key's `CN=Android Debug` (`edd22fdb…`), the arm64 `.so` is 29 MB,
all six third-party notices are in `assets/`, and no `.iso`, `.pak` or `.strg`
is in the package.

The port targets `versionName "0.1.0"` and `versionCode 1`.

## Per-platform packaging

| Platform | Builds from | Produces | State |
|---|---|---|---|
| Linux | `cmake -S . -B build-gcc` | executable | works; tests green. `cmake --install` also produces a complete tree, verified by running it |
| Linux | `tools/make_appimage.sh` | AppImage | builds; **notices missing** |
| Linux | `tools/make_flatpak.sh` | Flatpak | manifest installs a working tree and collects notices; **never built here** — no `flatpak-builder` on the development machine. The app id is a placeholder, and there is no AppStream metainfo |
| Windows | `.github/workflows/windows.yml` | zipped `dist/` | green in CI, artifact uploaded, packaged startup checked |
| Android | `tools/android_apk.sh :app:assembleRelease` | APK | builds, signed with this project's own key; **on-device behaviour unverified** |

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

1. ~~**Saving writes an empty save, on every platform.**~~ **Not a defect — it was
   the test harness, and saving and reloading both work.** The empty save came from
   a *card repair*, not a save: the in-game save screen found a file it considered
   corrupt, and answering that dialog deletes the file and re-creates it blank, with
   no save data. A save needs a second confirmation, at the screen's `SaveReady`
   state, and the driver was only ever pressing once. With that fixed, a save
   writes real data and the front end loads it — evidence in
   `docs/images/save-main-menu.png` (the main menu showing
   `[Samus A] 00% | Space Pirate Frigate | 00:00 Elapsed`) and
   `docs/images/save-loaded.png` (the loaded game, on the Frigate). The saved
   file's 3004-byte data region has 134 non-zero bytes, a valid CRC and slot 1
   flagged present.

   The one thing still unexplained is **why the in-game screen finds a corrupt
   file at all** on a card that was supposed to be empty. That is not a shipping
   blocker, but it is not understood, and `PORT_NOTES.md` records it as open.
2. **No answer for the decompiled game source.** The port's own work is MIT
   (`LICENSE`, scoped) and `NOTICE` says so, but what may be done with a working
   copy of decompiled game code is a question for the copyright holder, and no
   part of this tree answers it.
3. **The AppImage only *names* the shared libraries it copies** rather than
   reproducing their terms. Windows, AppImage, APK and the Flatpak manifest all
   collect the rest.
4. **Android on-device behaviour is unverified** — boot, render, save, play a
   seed and connect all need a device.
5. **`wss://` does not work on Android**: the NDK has no OpenSSL, so the
   build has no `MP_HAVE_OPENSSL` and a `wss://` server is refused rather than
   downgraded. A JNI `SSLSocket` backend or a vendored TLS library is what that
   needs.
6. **The Flatpak path has never been built.** Its manifest is now correct — the
   project's own install rules put a runnable tree in `/app`, and the notices
   are collected — but nothing here has ever run `flatpak-builder`, so the
   manifest is unproven against a real runtime. Three things also remain before
   it could be published, all of them decisions rather than code:
   - **The app id is a placeholder**, and it has to change. It also contains
     uppercase, which AppStream rejects for a component id
     (`cid-contains-uppercase-letter`), so the rename has to lowercase it.
   - **No screenshots.** Flathub requires them, and any screenshot of the running
     game shows Nintendo's game, which this package may not redistribute. This
     one cannot be fixed by writing a file.
   - The metainfo itself is in place at
     `packaging/org.metroidprime.MetroidPrimePort.metainfo.xml`, installed to
     `share/metainfo/` and passing `appstreamcli validate`; its only outstanding
     complaint is the component id's uppercase.
