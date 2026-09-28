// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argument, then $MP_DISC.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/gfx.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>

#include "port_debug.h"
#include "port_apclient.h"
#include "port_randomizer.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_build_info.h"
#include "port_log.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

extern "C" int metroid_main(int argc, char** argv);
extern "C" void AIPortShutdown(void);

namespace {
#if defined(__ANDROID__)
// Aurora logs to stderr, which Android discards. Send it to logcat instead so
// the Vulkan/audio/disc diagnostics are actually reachable on a device.
void AndroidLogCallback(AuroraLogLevel level, const char* module, const char* message,
                        unsigned int len) {
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG:
        priority = ANDROID_LOG_DEBUG;
        break;
    case LOG_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
    case LOG_ERROR:
        priority = ANDROID_LOG_ERROR;
        break;
    case LOG_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    case LOG_INFO:
    default:
        break;
    }
    __android_log_print(priority, "aurora", "[%s] %.*s", module != nullptr ? module : "",
                        static_cast< int >(len), message);
}
#endif

std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

bool IsDiscImage(const std::filesystem::path& path) {
    static const char* const kExtensions[] = {".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".nkit"};
    const std::string ext = LowerExtension(path);
    for (const char* candidate : kExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

// Looks for a disc image next to the executable (and in its immediate
// subdirectories) so a copied build is self-contained.
std::string FindDiscNextToExecutable() {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return {};
    }
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path baseDir(base);
    std::vector<fs::path> dirs{baseDir};
    for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec)) {
            dirs.push_back(it->path());
        }
    }
    for (const fs::path& dir : dirs) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_regular_file(ec) && IsDiscImage(it->path())) {
                return it->path().string();
            }
        }
    }
    return {};
}

const char* ResolveDiscPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return argv[i];
        }
    }
    if (const char* env = std::getenv("MP_DISC"); env != nullptr && env[0] != '\0') {
        return env;
    }
    if (const char* saved = PortDebug::DiscPath(); saved != nullptr) {
#if defined(__ANDROID__)
        if (std::strncmp(saved, "content://", 10) == 0) {
            // A URI from before the copy existed. Prefer the local copy, which
            // needs no permission grant; fall back to the URI if it is not there
            // yet, since the grant may still be live.
            char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
            if (pref != nullptr) {
                const std::string local = (std::filesystem::path(pref) / "disc.iso").string();
                SDL_free(pref);
                std::error_code ec;
                static const std::string sLocal =
                    std::filesystem::exists(local, ec) ? local : std::string();
                if (!sLocal.empty()) {
                    return sLocal.c_str();
                }
            }
            return saved;
        }
#endif
        if (std::filesystem::exists(saved)) {
            return saved;
        }
    }
    static const std::string sFound = FindDiscNextToExecutable();
    return sFound.empty() ? nullptr : sFound.c_str();
}

// aurora_dvd_open reports failure for three different reasons - the file would
// not open, the disc parser rejected it, or the data partition was missing -
// and says which of them nowhere. Splitting them here turns an unexplained
// exit into a specific, actionable line.
void ReportDiscOpenFailure(const char* path) {
    PortLog::Write( "metroid_prime_port: failed to open disc image: %s\n", path);
    SDL_ClearError();
    SDL_IOStream* probe = SDL_IOFromFile(path, "rb");
    if (probe == nullptr) {
        PortLog::Write( "  the file itself could not be opened: %s\n", SDL_GetError());
        return;
    }
    const Sint64 size = SDL_GetIOSize(probe);
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(probe, header, sizeof(header));
    SDL_CloseIO(probe);
    if (got != sizeof(header)) {
        PortLog::Write( "  the file opened but is only %lld bytes: too short to be a disc\n",
                        static_cast<long long>(size));
        return;
    }
    PortLog::Write( "  the file opened and is %lld bytes, so the disc parser rejected it\n",
                    static_cast<long long>(size));
    PortLog::Write( "  first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", header[0], header[1],
                    header[2], header[3], header[4], header[5], header[6], header[7]);
}

#if defined(__ANDROID__)
// Android's picker hands back a content:// URI, not a path. Opening one is
// possible (SDL routes SDL_IOFromFile through ContentResolver) but it depends
// on a permission grant that the system can revoke at any time - and on some
// builds the open fails even on the launch that picked the file. Copying the
// image into app storage once makes the remembered setting a plain file, which
// is readable with no grant at all and survives anything short of an uninstall.
//
// Returns the local copy's path, or an empty string if the copy failed.
std::string CopyDiscFromContentUri(const std::string& uri) {
    char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
    if (pref == nullptr) {
        PortLog::Write( "metroid_prime_port: no pref path to copy the disc into\n");
        return {};
    }
    const std::filesystem::path target = std::filesystem::path(pref) / "disc.iso";
    SDL_free(pref);

    SDL_IOStream* in = SDL_IOFromFile(uri.c_str(), "rb");
    if (in == nullptr) {
        PortLog::Write( "metroid_prime_port: could not read the picked image: %s: %s\n", uri.c_str(),
                        SDL_GetError());
        return {};
    }
    const Sint64 total = SDL_GetIOSize(in);
    SDL_IOStream* out = SDL_IOFromFile(target.string().c_str(), "wb");
    if (out == nullptr) {
        PortLog::Write( "metroid_prime_port: could not create %s: %s\n", target.string().c_str(),
                        SDL_GetError());
        SDL_CloseIO(in);
        return {};
    }

    char buffer[1 << 16];
    Sint64 done = 0;
    int lastPercent = -1;
    bool ok = true;
    for (;;) {
        const size_t got = SDL_ReadIO(in, buffer, sizeof(buffer));
        if (got == 0) {
            break;
        }
        if (SDL_WriteIO(out, buffer, got) != got) {
            PortLog::Write( "metroid_prime_port: writing %s failed: %s\n", target.string().c_str(),
                            SDL_GetError());
            ok = false;
            break;
        }
        done += static_cast<Sint64>(got);
        if (total > 0) {
            const int percent = static_cast<int>(done * 100 / total);
            // Every 5% rather than every chunk: a 1.5 GB image would otherwise
            // put 3000 lines in the log.
            if (percent / 5 != lastPercent / 5) {
                lastPercent = percent;
                PortLog::Write( "metroid_prime_port: copying the disc image, %d%%\n", percent);
            }
        }
    }
    if (!SDL_FlushIO(out)) {
        PortLog::Write( "metroid_prime_port: flushing %s failed: %s\n", target.string().c_str(),
                        SDL_GetError());
        ok = false;
    }
    if (!SDL_CloseIO(in)) {
        PortLog::Write( "metroid_prime_port: closing the picked image failed: %s\n", SDL_GetError());
    }
    if (!SDL_CloseIO(out)) {
        PortLog::Write( "metroid_prime_port: closing %s failed: %s\n", target.string().c_str(),
                        SDL_GetError());
        ok = false;
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(target, ec);
        return {};
    }
    // What landed on disk, not what was handed to the writer: a short write that
    // only fails at close looks identical to a good copy otherwise, and a
    // truncated image fails to parse as a disc with no further clue.
    std::error_code ec;
    const auto written = std::filesystem::file_size(target, ec);
    if (ec || static_cast<Sint64>(written) != done) {
        PortLog::Write( "metroid_prime_port: %s is %lld bytes on disk, expected %lld\n",
                        target.string().c_str(), static_cast<long long>(written),
                        static_cast<long long>(done));
        std::filesystem::remove(target, ec);
        return {};
    }
    PortLog::Write( "metroid_prime_port: copied the disc image to %s (%lld bytes)\n",
                    target.string().c_str(), static_cast<long long>(done));
    return target.string();
}
#endif  // __ANDROID__

// Asks for the disc image with the platform's file dialog and remembers the
// choice. SDL delivers the result on another thread, so this pumps events until
// it arrives; the callback also fires with an empty list if the dialog fails.
std::string AskForDiscImage() {
    static std::atomic< bool > answered{false};
    static std::string chosen;
    // Static because the callback cannot capture, but reset on every call: the
    // stale-disc retry asks a second time, and without this it would return the
    // first answer at once without showing a dialog. A first call only returns
    // early (timeout, quit, no window) on the way to exiting, so no callback
    // from it can still be pending here.
    answered.store(false);
    chosen.clear();
    const SDL_DialogFileFilter filters[] = {
        {"GameCube disc image", "iso;gcm;rvz;wbfs;ciso;nkit"},
        {"All files", "*"},
    };
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        // Headless, as on a build runner: nothing to show a dialog on, so say
        // no disc was given rather than waiting for an answer that cannot come.
        PortLog::Write( "metroid_prime_port: no window to ask for a disc image on\n");
        return {};
    }
    PortLog::Write( "metroid_prime_port: no disc image found; asking for one\n");
    SDL_ShowOpenFileDialog(
        [](void*, const char* const* files, int) {
            if (files != nullptr && files[0] != nullptr) {
                chosen = files[0];
            }
            answered.store(true);
        },
        nullptr, window, filters, 2, nullptr, false);
    // Wait for the answer, but not forever: a dialog that never calls back
    // would otherwise hang a scripted or headless run.
    const Uint64 deadline = SDL_GetTicks() + 5 * 60 * 1000;
    while (!answered.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                PortLog::Write( "metroid_prime_port: disc selection cancelled\n");
                return {};
            }
        }
        if (SDL_GetTicks() > deadline) {
            PortLog::Write( "metroid_prime_port: disc selection timed out\n");
            return {};
        }
        SDL_Delay(10);
    }
    if (!chosen.empty()) {
#if defined(__ANDROID__)
        // The picker returns a content:// URI. Copy it to a real file so that the
        // remembered setting needs no grant on the next launch.
        if (std::strncmp(chosen.c_str(), "content://", 10) == 0) {
            const std::string local = CopyDiscFromContentUri(chosen);
            if (!local.empty()) {
                chosen = local;
            }
        }
#endif
        PortDebug::SetDiscPath(chosen.c_str());
        // Persist immediately: the settings are otherwise only written from the
        // overlay's draw path, which never runs if the game cannot frame.
        PortDebug::SaveSettingsNow();
        PortLog::Write( "metroid_prime_port: disc image set to %s\n", chosen.c_str());
    }
    return chosen;
}

// Default texture-replacement folder next to the executable.
const char* DefaultTexturesPath() {
    static const std::string sPath = [] {
#if defined(__ANDROID__)
        char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
        if (pref == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(pref) + "textures";
        SDL_free(pref);
#else
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(base) + "textures";
#endif
        std::error_code ec;
        return std::filesystem::is_directory(dir, ec) ? dir : std::string();
    }();
    return sPath.empty() ? nullptr : sPath.c_str();
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("Metroid Prime native port %s\n", MP_BUILD_REVISION);
        return 0;
    }
    PortLog::Write( "metroid_prime_port: build %s\n", MP_BUILD_REVISION);
    PortRandomizer::EnsureLoaded();
    PortAp::EnsureLoaded();
    // A 16:9 window when widescreen is requested; the game's render mode is
    // widened to match. Values are the default window size only.
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    // MP_DUMP_TEXTURES=1 writes every source texture to
    // <cachePath>/texture_dumps as DDS, so replacement packs can be authored.
    const char* dumpEnv = std::getenv("MP_DUMP_TEXTURES");
    const bool dumpTextures = dumpEnv != nullptr && dumpEnv[0] != '\0' && std::strcmp(dumpEnv, "0") != 0;
    std::string resourcesPath;
#if defined(__ANDROID__)
    if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
        resourcesPath = pref;
        SDL_free(pref);
    }
#endif
    AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = std::getenv("MP_USER_PATH"),
        .cachePath = std::getenv("MP_CACHE_PATH"),
        .resourcesPath = resourcesPath.empty() ? nullptr : resourcesPath.c_str(),
        .desiredBackend = BACKEND_AUTO,
        .vsync = false,
        .allowTextureDumps = dumpTextures,
        // Keep the internal framebuffer at the game's logical size so its two
        // framebuffer allocations fit in MEM1; Aurora upscales to the window.
        .windowWidth = static_cast<uint32_t>(widescreen ? 854 : 640),
        .windowHeight = 480,
        .mem1Size = MEM1_DEFAULT_SIZE,
        .mem2Size = ARAM_DEFAULT_SIZE,
    };

#if defined(__ANDROID__)
    // SDL3 drops touch-derived mouse events by default, and ImGui's SDL3
    // backend only understands mouse events. The touch overlay in Java claims
    // gameplay touches, so whatever reaches SDL here is meant for ImGui.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    config.logCallback = AndroidLogCallback;
#endif
    // SDL3 reaches for its Wayland backend whenever a Wayland display is
    // reachable, and does so even without WAYLAND_DISPLAY — it falls back to the
    // default socket in XDG_RUNTIME_DIR. Under GNOME that backend never returns
    // from SDL_ShowWindow: it dispatches pending events, libdecor's client-side
    // decoration configure re-enters GTK layout from inside that dispatch, and the
    // process spins at 100% before the first frame is ever presented. Nothing in
    // the port can fix that, so whenever an X display is available ask for the X11
    // backend instead. SDL_VIDEODRIVER still wins, which is also how anyone who
    // wants Wayland opts back in.
    {
        const char* requested = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
        const char* x11 = std::getenv("DISPLAY");
        const auto present = [](const char* v) { return v != nullptr && v[0] != '\0'; };
        const bool unnamed = !present(requested);
        if (unnamed && present(x11)) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            PortLog::Write(
                         "port: using SDL's x11 video driver on %s; its wayland backend hangs on "
                         "window creation under GNOME (libdecor). Set SDL_VIDEODRIVER to "
                         "override.\n",
                         x11);
        } else if (unnamed) {
            // Nothing to fall back to: SDL will use Wayland, and on GNOME that is
            // the hang above. Say so before the silence, since there is no way to
            // tell from inside the hang.
            std::fputs("port: no DISPLAY set, so SDL will use its wayland backend, which hangs on "
                       "window creation under GNOME (libdecor). Run under an X display, or set "
                       "SDL_VIDEODRIVER yourself.\n",
                       stderr);
        }
    }
    aurora_initialize(argc, argv, &config);
    // Apply the persisted render scale. Vsync is applied on the first drawn
    // frame (once the swapchain surface exists) so it uses real capabilities.
    VISetFrameBufferScale(PortDebug::RenderScale());
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png); a per-device
    // subfolder is selected from the connected controller. Aurora also accepts
    // Dolphin format names such as CMPR and RGBA8.
    const char* textures = std::getenv("MP_TEXTURES");
    if (textures == nullptr || textures[0] == '\0') {
        textures = DefaultTexturesPath();
    }
    PortTextures::Initialize(textures);
    // Binding-aware prompt icons, served from <textures>/bindings.
    PortPrompts::Initialize(textures);

    // Disc image: an explicit argument or MP_DISC, else the path saved on a
    // previous launch, else a copy beside the executable, else ask for one.
    PortDebug::LoadDiscPath();
    std::string discImage;
    if (const char* resolved = ResolveDiscPath(argc, argv); resolved != nullptr) {
        discImage = resolved;
    } else {
        discImage = AskForDiscImage();
    }
    if (discImage.empty()) {
        PortLog::Write(
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC, or place the image next to the executable.\n", argv[0]);
        aurora_shutdown();
        return 1;
    }
    const char* discPath = discImage.c_str();

    if (!aurora_dvd_open(discPath)) {
        ReportDiscOpenFailure(discPath);
        // A remembered disc goes stale whenever its permission lapses: on Android
        // the provider can reclaim a persisted URI grant, and on desktop the file
        // may have been moved or deleted. Retrying once through the picker turns
        // an unexplained exit into a recoverable prompt.
        const bool fromArgs = argc > 1 || std::getenv("MP_DISC") != nullptr;
        // DiscPath() is null when no disc is remembered, and comparing a
        // std::string with a null pointer is undefined (it calls strlen(NULL)).
        const char* rememberedDisc = PortDebug::DiscPath();
        if (rememberedDisc != nullptr && discImage == rememberedDisc && !fromArgs) {
            PortLog::Write( "metroid_prime_port: asking for the disc image again\n");
            PortDebug::SetDiscPath("");
            PortDebug::SaveSettingsNow();
            discImage = AskForDiscImage();
            if (discImage.empty()) {
                PortLog::Write( "metroid_prime_port: no disc image given.\n"
                                "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n",
                                argv[0]);
                aurora_shutdown();
                return 1;
            }
            discPath = discImage.c_str();
            if (!aurora_dvd_open(discPath)) {
                ReportDiscOpenFailure(discPath);
                aurora_shutdown();
                return 1;
            }
        } else {
            aurora_shutdown();
            return 1;
        }
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);
    const DVDDiskID* discId = DVDGetCurrentDiskID();
    if (discId == nullptr || std::memcmp(discId->gameName, "GM8E", 4) != 0 ||
        std::memcmp(discId->company, "01", 2) != 0 || discId->diskNumber != 0 || discId->gameVersion != 0) {
        PortLog::Write( "metroid_prime_port: unsupported disc; expected GM8E01 USA revision 0.\n");
        aurora_dvd_close();
        aurora_shutdown();
        return 1;
    }

    // Prime the window/event state so the game's first aurora_begin_frame can
    // succeed (the game submits GX during early init, before its main loop).
    aurora_update();

    int result = 1;
    try {
        result = metroid_main(argc, argv);
    } catch (const std::exception& error) {
        PortLog::Write( "metroid_prime_port: %s\n", error.what());
    }

    AIPortShutdown();
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
