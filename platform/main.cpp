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
#include <aurora/texture.hpp>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>

#include "port_debug.h"
#include "port_build_info.h"

#include <SDL3/SDL_filesystem.h>

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
    static const std::string sFound = FindDiscNextToExecutable();
    return sFound.empty() ? nullptr : sFound.c_str();
}

// Default texture-replacement folder next to the executable.
const char* DefaultTexturesPath() {
    static const std::string sPath = [] {
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(base) + "textures";
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
    std::fprintf(stderr, "metroid_prime_port: build %s\n", MP_BUILD_REVISION);
    const char* discPath = ResolveDiscPath(argc, argv);
    if (discPath == nullptr) {
        std::fprintf(stderr,
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC, or place the image next to the executable.\n", argv[0]);
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
    // Apply the persisted render scale. Vsync is applied on the first drawn
    // frame (once the swapchain surface exists) so it uses real capabilities.
    VISetFrameBufferScale(PortDebug::RenderScale());
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png). Loaded once;
    // Aurora also accepts Dolphin format names such as CMPR and RGBA8.
    static aurora::texture::ReplacementGroup sTextureReplacements;
    const char* textures = std::getenv("MP_TEXTURES");
    if (textures == nullptr || textures[0] == '\0') {
        textures = DefaultTexturesPath();
    }
    if (textures != nullptr) {
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
