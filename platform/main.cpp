// Port entry point. Aurora supplies the real process entry (aurora_main) and the
// platform loop; this file boots the game and drives it.
//
// TODO: replace the placeholder loop with the game's boot (CMain / CMainFlow)
// and per-frame update once the decomp sources build against Aurora.

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h>

#include <cstdio>

int main(int argc, char* argv[]) {
    const AuroraConfig config = {
        .appName = "Metroid Prime",
    };
    aurora_initialize(argc, argv, &config);
    std::printf("metroid_prime_port: scaffold built; game boot not wired yet\n");
    aurora_shutdown();
    return 0;
}
