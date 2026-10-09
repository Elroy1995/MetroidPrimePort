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

#include "port_env.h"
#include "port_embedded.h"
#include "port_crash.h"
#include "port_debug.h"
#include "port_paths.h"
#include "port_actor_collision_bounds.h"
#include "port_apclient.h"
#include "port_randomizer.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_build_info.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_mods.h"
#include "port_room_geo.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_gpu_driver.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#include <sys/system_properties.h>
#endif
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/utsname.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

extern "C" int metroid_main(int argc, char** argv);
extern "C" void AIPortShutdown(void);

namespace {
#if defined(__ANDROID__)
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
    char line[2048];
    std::snprintf(line, sizeof(line), "[%s] %.*s", module != nullptr ? module : "", static_cast< int >(len),
                  message);
    __android_log_write(priority, "aurora", line);
    PortLogFile::Write("aurora", line);
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

enum class DiscMatch { No, Maybe, Yes };

DiscMatch MatchDiscImage(const std::filesystem::path& path) {
    if (!IsDiscImage(path)) {
        return DiscMatch::No;
    }
    const std::string ext = LowerExtension(path);
    if (ext != ".iso" && ext != ".gcm") {
        return DiscMatch::Maybe;
    }
    SDL_IOStream* file = SDL_IOFromFile(PortPaths::detail::ToUtf8(path).c_str(), "rb");
    if (file == nullptr) {
        return DiscMatch::No;
    }
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(file, header, sizeof(header));
    SDL_CloseIO(file);
    return got == sizeof(header) && std::memcmp(header, "GM8E01", 6) == 0 && header[6] == 0 && header[7] == 0
               ? DiscMatch::Yes
               : DiscMatch::No;
}

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
// iOS: Searches the sandboxed Documents directory where the user can place files via Files app
std::string FindDiscInIosDocuments() {
    namespace fs = std::filesystem;
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') {
        return {};
    }
    fs::path baseDir = fs::path(home) / "Documents";
    try {
        std::error_code ec;
        if (!fs::exists(baseDir, ec) || !fs::is_directory(baseDir, ec)) {
            return {};
        }
        std::string maybe;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_regular_file(entryEc) && IsDiscImage(it->path())) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const fs::path& file : files) {
            const DiscMatch match = MatchDiscImage(file);
            if (match == DiscMatch::Yes) {
                PortLog::Write("metroid_prime_port: found valid disc in iOS Documents: %s\n", file.string().c_str());
                return file.string();
            }
            if (match == DiscMatch::Maybe && maybe.empty()) {
                maybe = file.string();
            }
        }
        if (!maybe.empty()) {
            PortLog::Write("metroid_prime_port: using candidate disc in iOS Documents: %s\n", maybe.c_str());
            return maybe;
        }
    } catch (const std::exception& e) {
        PortLog::Write("metroid_prime_port: iOS Documents scan failed: %s\n", e.what());
    }
    return {};
}
#endif

std::string FindDiscNextToExecutable() {
#if defined(__ANDROID__)
    const char* rawBase = SDL_GetBasePath();
    const std::string base = rawBase != nullptr ? rawBase : "";
#else
    const std::string base = PortPaths::detail::ExecutableFolder();
#endif
    if (base.empty()) {
        return {};
    }
    namespace fs = std::filesystem;
    try {
        std::error_code ec;
        const fs::path baseDir = PortPaths::detail::FromUtf8(base);
        std::vector<fs::path> dirs{baseDir};
        for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                dirs.push_back(it->path());
            }
        }
        std::sort(dirs.begin() + 1, dirs.end());
        std::string maybe;
        for (const fs::path& dir : dirs) {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && IsDiscImage(it->path())) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                const DiscMatch match = MatchDiscImage(file);
                if (match == DiscMatch::Yes) {
                    return PortPaths::detail::ToUtf8(file);
                }
                if (match == DiscMatch::Maybe && maybe.empty()) {
                    maybe = PortPaths::detail::ToUtf8(file);
                }
            }
        }
        if (!maybe.empty()) {
            return maybe;
        }
    } catch (const std::exception& e) {
        PortLog::Write("metroid_prime_port: searching for a disc image failed: %s\n", e.what());
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
        std::error_code ec;
        if (std::filesystem::exists(saved, ec)) {
            return saved;
        }
    }

#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    // On iOS, check the user's accessible Documents folder first
    static const std::string sIosFound = FindDiscInIosDocuments();
    if (!sIosFound.empty()) {
        PortLog::Write("metroid_prime_port: using disc found in Documents: %s\n", sIosFound.c_str());
        return sIosFound.c_str();
    }
#endif

    static const std::string sFound = FindDiscNextToExecutable();
    if (sFound.empty()) {
        return nullptr;
    }
    PortLog::Write("metroid_prime_port: using the disc image next to the executable: %s\n", sFound.c_str());
    return sFound.c_str();
}

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

bool ResolveDiscFromArgs(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return true;
        }
    }
    const char* env = std::getenv("MP_DISC");
    return env != nullptr && env[0] != '\0';
}

bool IsSupportedId(const char* id6, unsigned diskNumber, unsigned version) {
    return std::memcmp(id6, "GM8E01", 6) == 0 && diskNumber == 0 && version == 0;
}

std::array<char, 6> DiscId6(const DVDDiskID& id) {
    std::array<char, 6> id6{};
    std::memcpy(id6.data(), id.gameName, 4);
    std::memcpy(id6.data() + 4, id.company, 2);
    return id6;
}

bool IsSupportedDisc(const DVDDiskID* id) {
    return id != nullptr && IsSupportedId(DiscId6(*id).data(), id->diskNumber, id->gameVersion);
}

std::string FindUnreadableDiscFile() {
    constexpr size_t kFilesToCheck = 8;
    const Uint64 start = SDL_GetTicks();
    const s32 count = aurora_dvd_base_entry_count();
    std::vector<std::pair<int64_t, s32>> ends;
    for (s32 entry = 1; entry < count; ++entry) {
        const int64_t offset = aurora_dvd_base_offset(entry);
        if (offset < 0) {
            continue;
        }
        void* file = aurora_dvd_base_open(entry);
        const int64_t size = file != nullptr ? aurora_dvd_base_seek(file, 0, SEEK_END) : -1;
        aurora_dvd_base_close(file);
        ends.emplace_back(offset + std::max<int64_t>(size, 0), entry);
    }
    std::sort(ends.begin(), ends.end(), std::greater<>());
    ends.resize(std::min(ends.size(), kFilesToCheck));
    for (const auto& [end, entry] : ends) {
        void* file = aurora_dvd_base_open(entry);
        const int64_t size = file != nullptr ? aurora_dvd_base_seek(file, 0, SEEK_END) : -1;
        uint8_t last = 0;
        const bool ok = size == 0 || (size > 0 && aurora_dvd_base_seek(file, size - 1, SEEK_SET) == size - 1 &&
                                      aurora_dvd_base_read(file, &last, 1) == 1);
        aurora_dvd_base_close(file);
        if (!ok) {
            char path[256] = {};
            return DVDConvertEntrynumToPath(entry, path, sizeof(path)) ? std::string(path) : "entry " + std::to_string(entry);
        }
    }
    PortLog::Write("metroid_prime_port: disc image is complete (its last %zu files read in %llu ms)\n", ends.size(),
                   static_cast<unsigned long long>(SDL_GetTicks() - start));
    return {};
}

constexpr const char* kSupportedDisc = "Only Metroid Prime for the GameCube, USA version 1.00\n(GM8E01, revision 0), is supported.";

std::string DescribeDisc(const char* id6, unsigned version) {
    char id[7] = {};
    for (int i = 0; i < 6; ++i) {
        const char c = id6[i];
        id[i] = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ? c : '?';
    }
    std::string text;
    if (std::memcmp(id, "GM8", 3) == 0) {
        const char* region = id[3] == 'E' ? "USA" : id[3] == 'P' ? "European" : id[3] == 'J' ? "Japanese" : "another";
        text = std::string("This is the ") + region + " release of Metroid Prime (" + id + ", revision " +
               std::to_string(version) + ").";
    } else {
        text = std::string("This disc image is not Metroid Prime (game id ") + id + ").";
    }
    return text;
}

std::string DescribeUnsupportedDisc(const DVDDiskID* id) {
    if (id == nullptr) {
        return "This disc image has no readable game id.";
    }
    return DescribeDisc(DiscId6(*id).data(), id->gameVersion);
}

bool AskPickDisc(const char* title, const std::string& message, const char* pickLabel, bool offerPick) {
    if (port::EnvFlag("MP_NO_DISC_DIALOG")) {
        return offerPick;
    }
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        return offerPick;
    }
    enum { kClose, kPick };
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, kPick, pickLabel},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | (offerPick ? 0u : SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT), kClose,
         "Close"},
    };
    SDL_MessageBoxData data{};
    data.flags = SDL_MESSAGEBOX_ERROR;
    data.window = window;
    data.title = title;
    data.message = message.c_str();
    data.numbuttons = offerPick ? 2 : 1;
    data.buttons = offerPick ? buttons : buttons + 1;
    int answer = -1;
    if (!SDL_ShowMessageBox(&data, &answer)) {
        PortLog::Write("metroid_prime_port: could not show the disc message: %s\n", SDL_GetError());
        return offerPick;
    }
    return offerPick && answer == kPick;
}

bool ShowDiscError(const std::string& problem, const std::string& path, bool askNext) {
    const std::string message = problem + "\n\n" + path + "\n\n" + kSupportedDisc;
    return AskPickDisc("Metroid Prime: wrong disc image", message, "Pick another disc", askNext);
}

void ForgetDisc(const std::string& path) {
    bool forget = false;
    if (const char* remembered = PortDebug::DiscPath(); remembered != nullptr && path == remembered) {
        forget = true;
    }
    if (forget) {
        PortDebug::SetDiscPath("");
        PortDebug::SaveSettingsNow();
        PortLog::Write("metroid_prime_port: forgot the remembered disc image\n");
    }
}

std::string DiscReadFailedMarker() {
    return PortPaths::UserFolder().empty() ? std::string() : PortPaths::UserFolder() + "disc-read-failed";
}

std::string s_mountedDisc;

void NoteDiscReadFailure() {
    PortLog::Write("metroid_prime_port: a read of the disc image failed\n");
    const std::string marker = DiscReadFailedMarker();
    if (marker.empty()) {
        return;
    }
    if (FILE* file = std::fopen(marker.c_str(), "wb"); file != nullptr) {
        std::fputs(s_mountedDisc.c_str(), file);
        std::fclose(file);
    }
}

bool DiscReadFailedLastTime(const std::string& path) {
    const std::string marker = DiscReadFailedMarker();
    if (marker.empty()) {
        return false;
    }
    std::ifstream in(marker, std::ios::binary);
    if (!in) {
        return false;
    }
    const std::string failed((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    PortDebug::NoteDiscReadFailedLastSession();
    return failed == path;
}

std::string AskForDiscImage(bool* cancelled = nullptr) {
    static std::atomic< bool > answered{false};
    static std::atomic< bool > dismissed{false};
    static std::string chosen;
    if (cancelled != nullptr) {
        *cancelled = false;
    }
    answered.store(false);
    dismissed.store(false);
    chosen.clear();
    static const SDL_DialogFileFilter filters[] = {
        {"Metroid Prime disc image (iso, gcm, rvz, wbfs, ciso, nkit)", "iso;gcm;rvz;wbfs;ciso;nkit"},
        {"All files", "*"},
    };
    if (port::EnvFlag("MP_NO_DISC_DIALOG")) {
        PortLog::Write( "metroid_prime_port: not asking for a disc image (MP_NO_DISC_DIALOG)\n");
        return {};
    }
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        PortLog::Write( "metroid_prime_port: no window to ask for a disc image on\n");
        return {};
    }
    PortLog::Write( "metroid_prime_port: no disc image found; asking for one\n");
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          "Select your Metroid Prime disc image (GameCube, USA, v1.00)");
    SDL_ShowFileDialogWithProperties(
        SDL_FILEDIALOG_OPENFILE,
        [](void*, const char* const* files, int) {
            if (files != nullptr && files[0] != nullptr) {
                chosen = files[0];
            }
            dismissed.store(files != nullptr && files[0] == nullptr);
            answered.store(true);
        },
        nullptr, props);
    SDL_DestroyProperties(props);
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
        aurora_release_lost_surface();
        SDL_Delay(10);
    }
    if (!chosen.empty()) {
        PortDebug::SetDiscPath(chosen.c_str());
        PortDebug::SaveSettingsNow();
        PortLog::Write( "metroid_prime_port: disc image set to %s\n", chosen.c_str());
    } else if (dismissed.load()) {
        PortLog::Write("metroid_prime_port: no disc image picked\n");
        if (cancelled != nullptr) {
            *cancelled = true;
        }
    }
    return chosen;
}

std::string PickDisc() {
    for (;;) {
        bool cancelled = false;
        std::string disc = AskForDiscImage(&cancelled);
        if (!disc.empty() || !cancelled) {
            return disc;
        }
        if (!AskPickDisc("Metroid Prime: no disc image", std::string("No disc image was picked.\n\n") + kSupportedDisc,
                         "Pick a disc", true)) {
            return {};
        }
    }
}

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
#if defined(_WIN32)
    if ((argc == 4 || argc == 5) && std::strcmp(argv[1], "--log-copy") == 0) {
        return PortLogFile::RunCopy(argv[2], argv[3], argc == 5 ? argv[4] : nullptr);
    }
    PortLogFile::AttachParentConsole();
#endif
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("Metroid Prime native port %s (%s)\n", MP_BUILD_VERSION, MP_BUILD_REVISION);
        return 0;
    }
    {
        const bool logFile = port::EnvFlag("MP_LOG_FILE", PortDebug::LogFile());
        if (logFile && !PortLogFile::Start()) {
            PortLog::Write("port: cannot write the log to %s\n", PortLogFile::Path().c_str());
        }
    }
    PortCrash::Install();
    PortLog::Write( "metroid_prime_port: version %s, build %s\n", MP_BUILD_VERSION, MP_BUILD_REVISION);
    for (const std::string& line : PortPaths::MigrationLog()) {
        PortLog::Write("port: portable data: %s\n", line.c_str());
    }
    PortRandomizer::EnsureLoaded();
    PortAp::EnsureLoaded();
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    const bool dumpTextures = port::EnvFlag("MP_DUMP_TEXTURES");
    std::string resourcesPath;
    const unsigned long kRoomGeoMem1MB = 256;
#if defined(__ANDROID__)
    const unsigned long kRoomGeoFrameBuffers = 6;
#else
    const unsigned long kRoomGeoFrameBuffers = 12;
#endif
    const bool roomGeometry = PortMods::HasRoomGeometry();
    uint32_t mem1Size = MEM1_DEFAULT_SIZE;
    {
        const int envMb = port::EnvInt("MP_MEM1_MB", -1);
        const unsigned long mb = envMb >= 0 ? static_cast<unsigned long>(envMb) : roomGeometry ? kRoomGeoMem1MB : 0;
        if (mb > MEM1_DEFAULT_SIZE / (1024 * 1024)) {
            mem1Size = static_cast<uint32_t>(std::min(mb, 1024UL) * 1024 * 1024);
            PortLog::Write("port: MEM1 arena raised to %u MB (%s)\n", mem1Size / (1024 * 1024),
                           envMb >= 0 ? "MP_MEM1_MB" : "room geometry");
        }
    }
#if defined(__ANDROID__)
    const unsigned long kResidentMiB = 128;
#else
    const unsigned long kResidentMiB = 256;
#endif
    const unsigned long kResidentFrameBuffers = 2;
    bool resident = roomGeometry && PortDebug::RoomGeoResidentAtStartup();
    resident = roomGeometry && port::EnvFlag("MP_ROOM_GEO_RESIDENT", resident);
    uint32_t residentMiB = 0;
    if (resident) {
        const int envMiB = port::EnvInt("MP_ROOM_GEO_RESIDENT_MB", -1);
        residentMiB = static_cast<uint32_t>(
            envMiB >= 0 ? std::clamp(static_cast<unsigned long>(envMiB), 16UL, 2048UL) : kResidentMiB);
    }
    uint32_t frameBufferScale = !roomGeometry ? 1 : resident ? kResidentFrameBuffers : kRoomGeoFrameBuffers;
    const int envFrameBuffers = port::EnvInt("MP_FRAME_BUFFERS", -1);
    if (envFrameBuffers >= 0) {
        frameBufferScale = static_cast<uint32_t>(std::clamp(static_cast<unsigned long>(envFrameBuffers), 1UL, 16UL));
    }
    if (frameBufferScale > 1) {
        PortLog::Write("port: frame buffers at %ux (%s)\n", frameBufferScale,
                       envFrameBuffers >= 0 ? "MP_FRAME_BUFFERS" : "room geometry");
    }
    const std::string& userFolder = PortPaths::UserFolder();
    const char* cacheEnv = std::getenv("MP_CACHE_PATH");
    const std::string& defaultCache = userFolder;
    const std::string cacheFolder = cacheEnv != nullptr && cacheEnv[0] != '\0' ? cacheEnv : defaultCache;
    PortLog::Write("port: user folder %s%s\n", userFolder.empty() ? "(none)" : userFolder.c_str(),
                   PortPaths::IsPortable() ? " (portable)" : "");

    const std::span<const uint8_t> embeddedSeed = PortEmbedded::Find("initial_pipeline_cache.db");
    static uint8_t windowIcon[] = {
#include "port_window_icon.inc"
    };

    AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = userFolder.empty() ? nullptr : userFolder.c_str(),
        .cachePath = cacheFolder.empty() ? nullptr : cacheFolder.c_str(),
        .resourcesPath = resourcesPath.empty() ? nullptr : resourcesPath.c_str(),
        .desiredBackend = port::EnvFlag("MP_OPENGLES", PortDebug::OpenGles()) ? BACKEND_OPENGLES : BACKEND_AUTO,
        .msaa = static_cast<uint32_t>(PortDebug::Msaa()),
        .maxTextureAnisotropy = static_cast<uint16_t>(PortDebug::Anisotropy()),
        .vsync = false,
        .startFullscreen = PortDebug::Fullscreen(),
        .allowJoystickBackgroundEvents = false,
        .pauseOnFocusLost = false,
        .allowTextureDumps = dumpTextures,
        .allowCpuAdapter = false,
        .windowPosX = -1,
        .windowPosY = -1,
        .windowWidth = static_cast<uint32_t>(widescreen ? 1280 : 960),
        .windowHeight = 720,
        .iconRGBA8 = windowIcon,
        .iconWidth = 64,
        .iconHeight = 64,
        .logCallback = nullptr,
        .logLevel = LOG_DEBUG,
        .imGuiInitCallback = nullptr,
        .mem1Size = mem1Size,
        .mem2Size = ARAM_DEFAULT_SIZE,
        .frameBufferScale = frameBufferScale,
        .residentGeometryMiB = residentMiB,
        .pipelineCacheSeedData = embeddedSeed.data(),
        .pipelineCacheSeedSize = embeddedSeed.size(),
        .vulkanLibraryDir = nullptr,
    };

    PortDebug::ApplyStorageClamp();
    aurora_initialize(argc, argv, &config);

    if (aurora_get_frame_buffer_scale() != frameBufferScale) {
        PortLog::Write("port: frame buffers at %ux, all this device allows\n", aurora_get_frame_buffer_scale());
    }
    PortRoomGeo::SetBuffersReady(aurora_get_frame_buffer_scale() > 1);
    if (resident) {
        const uint32_t got = aurora_get_resident_geometry_mib();
        PortRoomGeo::SetResident(got != 0);
        PortLog::Write("port: room geometry kept on the GPU in %u MiB\n", got);
    }
    VISetFrameBufferScale(PortDebug::RenderScale());
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    const char* textures = std::getenv("MP_TEXTURES");
    if (textures == nullptr || textures[0] == '\0') {
        textures = PortEmbedded::Under("textures/").empty() ? DefaultTexturesPath() : nullptr;
    }
    std::string userTextures;
    if (const char* env = std::getenv("MP_USER_TEXTURES"); env != nullptr && env[0] != '\0') {
        userTextures = env;
    } else if (!PortPaths::UserFolder().empty()) {
        userTextures = PortPaths::UserFolder() + "user_textures";
    }
    PortTextures::Initialize(textures, userTextures.c_str());
    PortPrompts::Initialize(textures);

    // Resolves disc: first non-flag arg, then MP_DISC, then saved, then iOS Documents, then next to executable
    PortDebug::LoadDiscPath();
    std::string discImage;
    if (const char* resolved = ResolveDiscPath(argc, argv); resolved != nullptr) {
        discImage = resolved;
    } else {
        discImage = PickDisc();
    }
    if (discImage.empty()) {
        PortLog::Write(
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC, or place the image in the app Documents folder.\n", argv[0]);
        aurora_shutdown();
        return 1;
    }
    const char* discPath = discImage.c_str();

    const bool discFromArgs = ResolveDiscFromArgs(argc, argv);
    for (;;) {
        std::string problem;
        if (!aurora_dvd_open(discPath)) {
            ReportDiscOpenFailure(discPath);
            problem = "This file could not be read as a GameCube disc image.";
        } else if (!IsSupportedDisc(DVDGetCurrentDiskID())) {
            problem = DescribeUnsupportedDisc(DVDGetCurrentDiskID());
            PortLog::Write("metroid_prime_port: unsupported disc: %s Expected GM8E01 revision 0.\n", problem.c_str());
            aurora_dvd_close();
        } else if (DiscReadFailedLastTime(discImage)) {
            PortLog::Write("metroid_prime_port: refused the disc image: a read of it failed last session\n");
            problem = "Part of this disc image couldn't be read last time, so it's damaged.\n"
                      "Copy the file again, or check it in Dolphin (Properties > Verify).";
            aurora_dvd_close();
        } else if (const std::string unreadable = FindUnreadableDiscFile(); !unreadable.empty()) {
            PortLog::Write("metroid_prime_port: disc image is incomplete: %s can't be read\n", unreadable.c_str());
            problem = "This disc image is incomplete or damaged: part of it (" + unreadable +
                      ") can't be read.\nIt's probably an interrupted download or copy. Copy the file again.";
            aurora_dvd_close();
        }
        if (problem.empty()) {
            break;
        }
        ForgetDisc(discImage);
        if (!ShowDiscError(problem, discImage, !discFromArgs)) {
            if (!discFromArgs) {
                PortLog::Write("metroid_prime_port: closed at the disc message\n");
            }
            aurora_shutdown();
            return 1;
        }
        PortLog::Write("metroid_prime_port: asking for the disc image again\n");
        discImage = PickDisc();
        if (discImage.empty()) {
            PortLog::Write("metroid_prime_port: no disc image given.\n");
            aurora_shutdown();
            return 1;
        }
        discPath = discImage.c_str();
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);
    s_mountedDisc = discImage;
    aurora_dvd_set_read_error_callback(NoteDiscReadFailure);

    if (PortRemastered::ApplyPendingImport()) {
        PortLog::Write("metroid_prime_port: installed the imported Remastered models\n");
    }
    PortMods::Initialize();

    PortActorCollisionBounds::Reset();
    aurora_update();

    int result = 1;
    try {
        result = metroid_main(argc, argv);
    } catch (const std::exception& error) {
        PortLog::Write( "metroid_prime_port: %s\n", error.what());
    }

    AIPortShutdown();
    PortRemastered::StopImport();
    PortActorCollisionBounds::Reset();
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
