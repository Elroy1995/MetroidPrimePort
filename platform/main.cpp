// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argv, else $MP_DISC, else the default
// path used during development.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int metroid_main(int argc, char** argv);

namespace {
const char* kDefaultDisc = "/home/odran/rom/Metroid Prime (USA) (v1.00).iso";

const char* ResolveDiscPath(int argc, char** argv) {
    if (argc > 1 && argv[1][0] != '-') {
        return argv[1];
    }
    if (const char* env = std::getenv("MP_DISC")) {
        return env;
    }
    return kDefaultDisc;
}
} // namespace

int main(int argc, char** argv) {
    const AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = nullptr,
        .cachePath = nullptr,
        .resourcesPath = nullptr,
        .desiredBackend = BACKEND_AUTO,
        .mem1Size = MEM1_DEFAULT_SIZE,
        .mem2Size = ARAM_DEFAULT_SIZE,
    };

    aurora_initialize(argc, argv, &config);

    const char* discPath = ResolveDiscPath(argc, argv);
    if (!aurora_dvd_open(discPath)) {
        std::fprintf(stderr, "metroid_prime_port: failed to open disc image: %s\n", discPath);
        aurora_shutdown();
        return 1;
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);

    // Prime the window/event state so the game's first aurora_begin_frame can
    // succeed (the game submits GX during early init, before its main loop).
    aurora_update();

    const int result = metroid_main(argc, argv);

    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
