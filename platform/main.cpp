// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argument, then $MP_DISC.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <aurora/texture.hpp>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>

#include "port_debug.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

extern "C" int metroid_main(int argc, char** argv);
extern "C" void AIPortShutdown(void);

namespace {
const char* ResolveDiscPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return argv[i];
        }
    }
    if (const char* env = std::getenv("MP_DISC"); env != nullptr && env[0] != '\0') {
        return env;
    }
    return nullptr;
}
} // namespace

int main(int argc, char** argv) {
    const char* discPath = ResolveDiscPath(argc, argv);
    if (discPath == nullptr) {
        std::fprintf(stderr,
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC to the image path.\n", argv[0]);
        return 1;
    }
    // A 16:9 window when widescreen is requested; the game's render mode is
    // widened to match. Values are the default window size only.
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    const AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = std::getenv("MP_USER_PATH"),
        .cachePath = std::getenv("MP_CACHE_PATH"),
        .resourcesPath = nullptr,
        .desiredBackend = BACKEND_AUTO,
        .vsync = false,
        // Keep the internal framebuffer at the game's logical size so its two
        // framebuffer allocations fit in MEM1; Aurora upscales to the window.
        .windowWidth = static_cast<uint32_t>(widescreen ? 854 : 640),
        .windowHeight = 480,
        .mem1Size = MEM1_DEFAULT_SIZE,
        .mem2Size = ARAM_DEFAULT_SIZE,
    };

    aurora_initialize(argc, argv, &config);
    VISetFrameBufferScale(1.f);
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png). Loaded once;
    // Aurora also accepts Dolphin format names such as CMPR and RGBA8.
    static aurora::texture::ReplacementGroup sTextureReplacements;
    if (const char* textures = std::getenv("MP_TEXTURES")) {
        sTextureReplacements = aurora::texture::load_replacement_directory(textures);
        std::fprintf(stderr, "metroid_prime_port: loaded %zu texture replacements from %s\n",
                     sTextureReplacements.registrations.size(), textures);
    }

    if (!aurora_dvd_open(discPath)) {
        std::fprintf(stderr, "metroid_prime_port: failed to open disc image: %s\n", discPath);
        aurora_shutdown();
        return 1;
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);
    const DVDDiskID* discId = DVDGetCurrentDiskID();
    if (discId == nullptr || std::memcmp(discId->gameName, "GM8E", 4) != 0 ||
        std::memcmp(discId->company, "01", 2) != 0 || discId->diskNumber != 0 || discId->gameVersion != 0) {
        std::fprintf(stderr, "metroid_prime_port: unsupported disc; expected GM8E01 USA revision 0.\n");
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
        std::fprintf(stderr, "metroid_prime_port: %s\n", error.what());
    }

    AIPortShutdown();
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
