// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"
#include "port_apclient.h"
#include "port_controls.h"
#include "port_prompts.h"
#include "port_mouse.h"
#include "port_textures.h"
#include "port_build_info.h"
#if defined(__ANDROID__)
#include "touch_pad.h"
#endif

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CMapWorld.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "Kyoto/CResFactory.hpp"

#include <aurora/gfx.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <musyx/port_voices.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_sensor.h>
#include <SDL3/SDL_stdinc.h>
#include <SDL3/SDL_video.h>

#if defined(__ANDROID__)
#include <jni.h>
#include <android/log.h>
#include <SDL3/SDL_joystick.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace aurora {
void request_screenshot() noexcept;
}

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" int AIPortOutputEnabled(void);
extern "C" void salSetMuted(int muted);

namespace {
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
unsigned sSimRate = 60;
bool sSimAdaptive = false;
float sTickPeriod = 1.f / 60.f;
bool sFrameLimitEnabled = true;
bool sTurbo = false;
unsigned sTurboTicks = 1;
bool sTraceTiming = false;
uint64_t sTimingNs = 0;
// Wall-clock for the same span as sTimingNs, so presented frames can be divided
// by elapsed time rather than by the frames' own cost. Zero on the first call,
// which is why the delta is skipped until a previous reading exists.
uint64_t sTimingWallNs = 0;
uint64_t sTimingWallLastNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
double sActualFps = 0.0, sActualTps = 0.0;
double sThroughputFps = 0.0;
bool sVsyncEnabled = false;
float sRenderScale = 1.f;
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_4_3;
bool sHudWide = false;
bool sMouseAim = false;
bool sTwinStick = false;
float sStickAimRate = 900.f;
// Gyro aiming: off / hold / always, auto / controller / phone, and how fast a
// rotation turns into aim travel.
int sGyroMode = 0;
int sGyroSource = 0;
float sGyroRate = 600.f;
bool sMouseCaptured = false;
bool sMouseGameplayActive = false;
bool sMouseInvertX = false;
bool sMouseInvertY = false;
bool sMouseButtons = true;
bool sMouseCrosshair = true;
PortMouse::AimState sMouseAimState;
PortMouse::ButtonGate sMouseButtonGate;
PortMouse::HeldButtons sMouseHeldButtons;
float sMouseSensitivity = 0.0035f;
float sMousePendingX = 0.f;
float sMousePendingY = 0.f;
float sMouseFrameX = 0.f;
float sMouseFrameY = 0.f;
bool sAiAudioEnabled = true;
bool sMusyxAudioEnabled = true;
bool sResetRequested = false;
std::atomic< bool > sToggleRequested{false};
// Mirrors sVisible for readers on other threads, so they never touch the lazy
// initialization or the ImGui state owned by the game thread.
std::atomic< bool > sOverlayVisible{false};
// Same idea for the twin-stick setting, which the Android touch overlay uses to
// pick a controller layout.
std::atomic< bool > sTwinStickFlag{false};
// Set when a real pad, keyboard or mouse is used; the Android touch overlay takes
// it to get out of the way.
std::atomic< bool > sPhysicalInput{false};
// Gyro state: the pad's gyro sensor is enabled once, and the phone's sensor is
// looked up once, so neither is touched on every tick.
bool sControllerGyroEnabled = false;
bool sPhoneGyroSearched = false;
SDL_Sensor* sPhoneGyro = nullptr;
const char* sGyroStatus = "off";
bool sVisible = false;
bool sSettingsDirty = false;
bool sAudioSettingsApplied = false;
bool sPresentationSettingsApplied = false;
CStateManager* sStateManager = nullptr;
int sPendingTeleport = -1;
bool sHasWorldTeleport = false;
std::string sDiscPath;
uint32_t sWorldTeleportWorld = 0;
uint32_t sWorldTeleportArea = 0;
bool sWorldSweepRequested = false;
struct WorldSweep {
  std::vector< uint32_t > worlds;
  std::vector< uint32_t > areas;
  size_t world = 0;
  size_t area = 0;
  unsigned settledTicks = 0;
  unsigned stalledTicks = 0;
  unsigned completedAreas = 0;
  bool active = false;
  bool waiting = false;
  // An area can hold several layers, and only the active ones are built, so a
  // pickup behind a layer the save has not unlocked is not in the dump at all.
  // The tour revisits each area once per layer with a different one active,
  // which is what makes the dump cover the whole area rather than the state the
  // save happens to be in.
  int layer = 0;
  int layerCount = 1;
  unsigned layerPasses = 0;
  // Set between the hop away from an area and the hop back to it: the area has
  // to be gone before it is rebuilt with the next layer.
  bool revisiting = false;
} sWorldSweep;

std::string SettingsFilePath() {
  std::string dir;
  if (const char* env = std::getenv("MP_USER_PATH")) {
    if (env[0] != '\0') {
      dir = env;
    }
  }
  if (dir.empty()) {
    if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
      dir = pref;
      SDL_free(pref);
    } else {
      dir = ".";
    }
  }
  if (!dir.empty() && dir.back() != '/' && dir.back() != '\\') {
    dir += '/';
  }
  return dir + "port_settings.ini";
}

bool ParseBool(const std::string& value) {
  return value == "1" || value == "true" || value == "on" || value == "yes";
}

std::string Trim(const std::string& text) {
  const size_t begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return std::string();
  }
  const size_t end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

void MarkDirty() { sSettingsDirty = true; }

void ApplySetting(const std::string& key, const std::string& value) {
  if (key == "frame_limit") {
    sFrameLimitEnabled = ParseBool(value);
  } else if (key == "vsync") {
    sVsyncEnabled = ParseBool(value);
  } else if (key == "disc_path") {
    sDiscPath = value;
  } else if (key == "render_scale") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 0.f && f <= 4.f) {
      sRenderScale = f;
    }
  } else if (key == "aspect") {
    if (value == "16:9") {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (value == "window") {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (value == "4:3") {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (key == "hud_wide") {
    sHudWide = ParseBool(value);
  } else if (key == "mouse_aim") {
    sMouseAim = ParseBool(value);
  } else if (key == "twin_stick") {
    sTwinStick = ParseBool(value);
  } else if (key == "stick_aim_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 50.f && f <= 4000.f) {
      sStickAimRate = f;
    }
  } else if (key == "gyro_mode") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroMode = static_cast< int >(v);
    }
  } else if (key == "gyro_source") {
    const long v = std::strtol(value.c_str(), nullptr, 10);
    if (v >= 0 && v <= 2) {
      sGyroSource = static_cast< int >(v);
    }
  } else if (key == "gyro_rate") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 20.f && f <= 5000.f) {
      sGyroRate = f;
    }
  } else if (key == "mouse_invert_x") {
    sMouseInvertX = ParseBool(value);
  } else if (key == "mouse_invert_y") {
    sMouseInvertY = ParseBool(value);
  } else if (key == "mouse_buttons") {
    sMouseButtons = ParseBool(value);
  } else if (key == "mouse_crosshair") {
    sMouseCrosshair = ParseBool(value);
  } else if (key == "mouse_sensitivity") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f > 0.f) {
      sMouseSensitivity = f;
    }
  } else if (key == "skip_cutscenes") {
    sSkipCutscenes = ParseBool(value);
  } else if (key == "cutscene_speed") {
    const float f = static_cast< float >(std::atof(value.c_str()));
    if (std::isfinite(f) && f >= 1.f && f <= 32.f) {
      sCutsceneSpeed = f;
    }
  } else if (key == "sim_rate") {
    const long rate = std::strtol(value.c_str(), nullptr, 10);
    if (rate >= 30 && rate <= 480) {
      sSimRate = static_cast< unsigned >(rate);
    }
  } else if (key == "sim_adaptive") {
    sSimAdaptive = ParseBool(value);
  } else if (key == "ai_audio") {
    sAiAudioEnabled = ParseBool(value);
  } else if (key == "musyx_audio") {
    sMusyxAudioEnabled = ParseBool(value);
  } else if (key == "voices_muted") {
    MusyxPortClearSampleMutes();
    const char* cursor = value.c_str();
    while (*cursor != '\0') {
      char* end = nullptr;
      const unsigned long id = std::strtoul(cursor, &end, 10);
      if (end == cursor) {
        break;
      }
      MusyxPortSetSampleMuted(static_cast< unsigned >(id), 1);
      cursor = end;
      while (*cursor == ',' || *cursor == ' ') {
        ++cursor;
      }
    }
  }
}

void LoadSettings() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::fprintf(stderr, "metroid_prime_port: loaded settings from %s\n", path.c_str());
  std::string line;
  while (std::getline(file, line)) {
    const size_t comment = line.find('#');
    if (comment != std::string::npos) {
      line.erase(comment);
    }
    const size_t separator = line.find('=');
    if (separator == std::string::npos) {
      continue;
    }
    const std::string key = Trim(line.substr(0, separator));
    const std::string value = Trim(line.substr(separator + 1));
    if (!key.empty()) {
      ApplySetting(key, value);
    }
  }
}

void SaveSettings() {
  if (!sInitialized || !sSettingsDirty) {
    return;
  }
  const std::string path = SettingsFilePath();
  std::ofstream file(path, std::ios::trunc);
  if (!file.is_open()) {
    std::fprintf(stderr, "metroid_prime_port: could not write settings to %s\n", path.c_str());
    return;
  }
  const char* aspect = sAspectMode == PortDebug::kAspect_16_9  ? "16:9"
                       : sAspectMode == PortDebug::kAspect_Window ? "window"
                                                                  : "4:3";
  file << "# Metroid Prime native port settings. Written by the F1 debug overlay.\n";
  file << "# Environment variables (MP_*) override these for a single run.\n";
  file << "aspect=" << aspect << '\n';
  file << "hud_wide=" << (sHudWide ? 1 : 0) << '\n';
  file << "vsync=" << (sVsyncEnabled ? 1 : 0) << '\n';
  file << "render_scale=" << sRenderScale << '\n';
  file << "frame_limit=" << (sFrameLimitEnabled ? 1 : 0) << '\n';
  file << "skip_cutscenes=" << (sSkipCutscenes ? 1 : 0) << '\n';
  file << "cutscene_speed=" << sCutsceneSpeed << '\n';
  file << "sim_rate=" << sSimRate << '\n';
  file << "sim_adaptive=" << (sSimAdaptive ? 1 : 0) << '\n';
  file << "mouse_aim=" << (sMouseAim ? 1 : 0) << '\n';
  file << "twin_stick=" << (sTwinStick ? 1 : 0) << '\n';
  file << "stick_aim_rate=" << sStickAimRate << '\n';
  file << "gyro_mode=" << sGyroMode << '\n';
  file << "gyro_source=" << sGyroSource << '\n';
  file << "gyro_rate=" << sGyroRate << '\n';
  file << "mouse_invert_x=" << (sMouseInvertX ? 1 : 0) << '\n';
  file << "mouse_invert_y=" << (sMouseInvertY ? 1 : 0) << '\n';
  file << "mouse_buttons=" << (sMouseButtons ? 1 : 0) << '\n';
  file << "mouse_crosshair=" << (sMouseCrosshair ? 1 : 0) << '\n';
  file << "mouse_sensitivity=" << sMouseSensitivity << '\n';
  if (!sDiscPath.empty()) {
    file << "disc_path=" << sDiscPath << '\n';
  }
  file << "ai_audio=" << (sAiAudioEnabled ? 1 : 0) << '\n';
  file << "musyx_audio=" << (sMusyxAudioEnabled ? 1 : 0) << '\n';
  unsigned muted[64];
  const int mutedCount = MusyxPortGetMutedSamples(muted, 64);
  if (mutedCount > 0) {
    file << "voices_muted=";
    for (int i = 0; i < mutedCount; ++i) {
      file << (i == 0 ? "" : ",") << muted[i];
    }
    file << '\n';
  }
  file.flush();
  std::fprintf(stderr, "metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
}

// Whether the player just used a real pad, keyboard or mouse. The touch overlay
// is itself a virtual pad, and touches also arrive as a mouse, so neither counts.
// A stick or trigger has to move well past rest, so drift does not count either.
bool IsPhysicalInput(const SDL_Event& event) {
  switch (event.type) {
  case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    return !SDL_IsJoystickVirtual(event.gbutton.which);
  case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    return std::abs(static_cast< int >(event.gaxis.value)) > 16000 &&
           !SDL_IsJoystickVirtual(event.gaxis.which);
  case SDL_EVENT_KEY_DOWN:
    // The soft keyboard types into the overlay's text fields, and Back and the
    // media keys come from the phone itself.
    if (event.key.repeat || sOverlayVisible.load(std::memory_order_acquire)) {
      return false;
    }
    switch (event.key.scancode) {
    case SDL_SCANCODE_AC_BACK:
    case SDL_SCANCODE_VOLUMEUP:
    case SDL_SCANCODE_VOLUMEDOWN:
    case SDL_SCANCODE_MUTE:
    case SDL_SCANCODE_MEDIA_PLAY:
    case SDL_SCANCODE_MEDIA_PAUSE:
    case SDL_SCANCODE_MEDIA_PLAY_PAUSE:
    case SDL_SCANCODE_MEDIA_NEXT_TRACK:
    case SDL_SCANCODE_MEDIA_PREVIOUS_TRACK:
    case SDL_SCANCODE_MEDIA_STOP:
    case SDL_SCANCODE_POWER:
      return false;
    default:
      return true;
    }
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
    return event.button.which != SDL_TOUCH_MOUSEID && event.button.which != SDL_PEN_MOUSEID;
  default:
    return false;
  }
}

bool SDLCALL debug_event_watch(void*, SDL_Event* event) {
  // F1 toggles the overlay. Watch the event rather than polling the key state:
  // a short tap can begin and end between two frames, so polling misses it.
  if (event->type == SDL_EVENT_KEY_DOWN && !event->key.repeat &&
      event->key.scancode == SDL_SCANCODE_F1) {
    PortDebug::RequestToggle();
  }
  if (IsPhysicalInput(*event)) {
    sPhysicalInput.store(true, std::memory_order_release);
  }
  return true;
}

void EnsureInitialized() {
  if (sInitialized) {
    return;
  }
  sInitialized = true;
  LoadSettings();

  // Environment variables are explicit per-run overrides and win over the file.
  if (std::getenv("MP_TRACE_TIMING") != nullptr) {
    sTraceTiming = true;
  }
  if (std::getenv("MP_FAST_BOOT") != nullptr) {
    sFastBoot = true;
  }
  if (const char* turbo = std::getenv("MP_TURBO")) {
    sTurbo = true;
    const long ticks = std::strtol(turbo, nullptr, 10);
    if (ticks >= 1 && ticks <= 16) {
      sTurboTicks = static_cast< unsigned >(ticks);
    }
  }
  if (std::getenv("MP_SKIP_CUTSCENES") != nullptr) {
    sSkipCutscenes = true;
  }
  if (std::getenv("MP_SHOW_DEBUG_UI") != nullptr) {
    sVisible = true;
  }
  if (const char* aspect = std::getenv("MP_ASPECT")) {
    if (std::strcmp(aspect, "16:9") == 0) {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (std::strcmp(aspect, "window") == 0) {
      sAspectMode = PortDebug::kAspect_Window;
    } else if (std::strcmp(aspect, "4:3") == 0) {
      sAspectMode = PortDebug::kAspect_4_3;
    }
  } else if (std::getenv("MP_WIDESCREEN") != nullptr) {
    sAspectMode = PortDebug::kAspect_16_9;
  }
  if (std::getenv("MP_HUD_WIDE") != nullptr) {
    sHudWide = true;
  }
  if (std::getenv("MP_MOUSE_AIM") != nullptr) {
    sMouseAim = true;
  }
  if (std::getenv("MP_TWIN_STICK") != nullptr) {
    sTwinStick = true;
  }
  if (std::getenv("MP_MOUSE_INVERT_X") != nullptr) {
    sMouseInvertX = true;
  }
  if (std::getenv("MP_MOUSE_INVERT_Y") != nullptr) {
    sMouseInvertY = true;
  }
  if (std::getenv("MP_DISABLE_MOUSE_BUTTONS") != nullptr) {
    sMouseButtons = false;
  }
  if (std::getenv("MP_DISABLE_MOUSE_CROSSHAIR") != nullptr) {
    sMouseCrosshair = false;
  }
  if (const char* sens = std::getenv("MP_MOUSE_SENS")) {
    const float value = static_cast< float >(std::atof(sens));
    if (std::isfinite(value) && value > 0.f) {
      sMouseSensitivity = value;
    }
  }
  if (std::getenv("MP_DISABLE_AI_AUDIO") != nullptr) {
    sAiAudioEnabled = false;
  }
  if (const char* speed = std::getenv("MP_CUTSCENE_SPEED")) {
    const float value = static_cast< float >(std::atof(speed));
    if (std::isfinite(value) && value >= 1.f && value <= 32.f) {
      sCutsceneSpeed = value;
    }
  }
  if (const char* rate = std::getenv("MP_SIM_RATE")) {
    const long value = std::strtol(rate, nullptr, 10);
    if (value >= 30 && value <= 480) {
      sSimRate = static_cast< unsigned >(value);
    }
  }
  if (std::getenv("MP_SIM_ADAPTIVE") != nullptr) {
    sSimAdaptive = true;
  }

  std::atexit(SaveSettings);
}
} // namespace

namespace PortDebug {

bool FastBoot() {
  EnsureInitialized();
  return sFastBoot;
}

bool SkipCutscenes() {
  EnsureInitialized();
  return sSkipCutscenes;
}

void SetSkipCutscenes(bool enabled) {
  EnsureInitialized();
  sSkipCutscenes = enabled;
  MarkDirty();
}

float CutsceneSpeed() {
  EnsureInitialized();
  return sCutsceneSpeed;
}

unsigned SimRate() {
  EnsureInitialized();
  return sSimRate;
}

void SetSimRate(unsigned hz) {
  EnsureInitialized();
  if (hz < 30u || hz > 480u) {
    return;
  }
  sSimRate = hz;
  MarkDirty();
}

float SimPeriod() { return 1.f / static_cast< float >(SimRate()); }

bool SimAdaptive() {
  EnsureInitialized();
  return sSimAdaptive;
}

bool Turbo() {
  EnsureInitialized();
  return sTurbo;
}

unsigned TurboTicks() {
  EnsureInitialized();
  return sTurbo ? sTurboTicks : 1;
}

void SetSimAdaptive(bool enabled) {
  EnsureInitialized();
  sSimAdaptive = enabled;
  MarkDirty();
}

float TickPeriod() {
  EnsureInitialized();
  return sTickPeriod;
}

void SetTickPeriod(float dt) {
  if (std::isfinite(dt) && dt > 0.f) {
    sTickPeriod = dt;
  }
}

float TickFrames() { return sTickPeriod * 60.f; }

bool FrameLimitEnabled() {
  EnsureInitialized();
  return sFrameLimitEnabled;
}

void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented) {
  sTimingNs += durationNs;
  sTimingTicks += ticks;
  if (presented) ++sTimingFrames;
  // Wall-clock time for the same span, kept separately from the frame's own
  // duration. Dividing presented frames by the frame's CPU time reports
  // throughput, which is what the process can *produce*; dividing by wall time
  // reports what reaches the screen. The two agree while the 60 Hz cap is
  // waiting more than the frame costs, and part company exactly when a frame
  // overruns its budget - which is the case someone opens the Performance tab
  // to diagnose. One of the two numbers without the other is misleading there.
  {
    const uint64_t nowNs = SDL_GetTicksNS();
    if (sTimingWallLastNs != 0) {
      sTimingWallNs += nowNs - sTimingWallLastNs;
    }
    sTimingWallLastNs = nowNs;
  }
  if (sTimingNs >= 1000000000ull) {
    const double seconds = static_cast<double>(sTimingNs) / 1000000000.0;
    const double wallSeconds = static_cast<double>(sTimingWallNs) / 1000000000.0;
    sActualFps = sTimingFrames / (wallSeconds > 0.0 ? wallSeconds : seconds);
    sThroughputFps = sTimingFrames / seconds;
    sActualTps = sTimingTicks / seconds;
    if (sTraceTiming) {
      std::fprintf(stderr,
                   "[timing] presented=%.1f FPS throughput=%.1f FPS simulation=%.1f ticks/s cap=%s\n",
                   sActualFps, sThroughputFps, sActualTps, sFrameLimitEnabled ? "60" : "off");
    }
    sTimingNs = 0;
    sTimingWallNs = 0;
    sTimingFrames = sTimingTicks = 0;
  }
}

void SetFrameLimitEnabled(bool enabled) {
  EnsureInitialized();
  if (sFrameLimitEnabled != enabled) {
    sFrameLimitEnabled = enabled;
    MarkDirty();
  }
}

bool VsyncEnabled() {
  EnsureInitialized();
  return sVsyncEnabled;
}

void SetVsyncEnabled(bool enabled) {
  EnsureInitialized();
  // Always re-apply: the stored value can match while the surface still has the
  // previous present mode (e.g. the persisted value applied before the first
  // frame), which made the first toggle a no-op.
  sVsyncEnabled = enabled;
  aurora_enable_vsync(enabled);
}

float RenderScale() {
  EnsureInitialized();
  return sRenderScale;
}

void SetRenderScale(float scale) {
  EnsureInitialized();
  if (sRenderScale == scale) {
    return;
  }
  sRenderScale = scale;
  VISetFrameBufferScale(scale);
}

EAspectMode AspectMode() {
  EnsureInitialized();
  return sAspectMode;
}

void SetAspectMode(EAspectMode mode) {
  EnsureInitialized();
  sAspectMode = mode;
  MarkDirty();
}

bool HudWide() {
  EnsureInitialized();
  return sHudWide;
}

void SetHudWide(bool enabled) {
  EnsureInitialized();
  sHudWide = enabled;
  MarkDirty();
}

bool MouseAim() {
  EnsureInitialized();
  return sMouseAim;
}

void SetMouseAim(bool enabled) {
  EnsureInitialized();
  sMouseAim = enabled;
  ResetMouseAim();
}

bool TwinStick() {
  EnsureInitialized();
  return sTwinStick;
}

void SetTwinStick(bool enabled) {
  EnsureInitialized();
  sTwinStick = enabled;
  MarkDirty();
}

float StickAimRate() {
  EnsureInitialized();
  return sStickAimRate;
}

void SetStickAimRate(float pixelsPerSecond) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecond) && pixelsPerSecond >= 50.f && pixelsPerSecond <= 4000.f) {
    sStickAimRate = pixelsPerSecond;
    MarkDirty();
  }
}

void AddStickAim(float x, float y, float dt) {
  EnsureInitialized();
  if (!sTwinStick || Visible() || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(dt) ||
      dt <= 0.f) {
    return;
  }
  // x right / y up; the aim state expects SDL-style right/down positive.
  sMouseFrameX += x * sStickAimRate * dt;
  sMouseFrameY -= y * sStickAimRate * dt;
}

int GyroMode() {
  EnsureInitialized();
  return sGyroMode;
}

void SetGyroMode(int mode) {
  EnsureInitialized();
  if (mode < 0 || mode > 2) {
    return;
  }
  sGyroMode = mode;
  MarkDirty();
}

int GyroSource() {
  EnsureInitialized();
  return sGyroSource;
}

void SetGyroSource(int source) {
  EnsureInitialized();
  if (source < 0 || source > 2) {
    return;
  }
  sGyroSource = source;
  MarkDirty();
}

float GyroRate() {
  EnsureInitialized();
  return sGyroRate;
}

void SetGyroRate(float pixelsPerSecondPerRad) {
  EnsureInitialized();
  if (std::isfinite(pixelsPerSecondPerRad) && pixelsPerSecondPerRad >= 20.f &&
      pixelsPerSecondPerRad <= 5000.f) {
    sGyroRate = pixelsPerSecondPerRad;
    MarkDirty();
  }
}

const char* GyroStatus() { return sGyroStatus; }

void PollGyro() {
  EnsureInitialized();
  if (sGyroMode == 0) {
    sGyroStatus = "off";
    return;
  }
  // Gyro feeds the same aim state the mouse and twin stick use, so it only has
  // an effect where that is driving the camera.
  if (!sMouseAim && !sTwinStick) {
    sGyroStatus = "needs mouse aim or twin stick";
    return;
  }

  const bool wantController = sGyroSource == 0 || sGyroSource == 1;
  const bool wantPhone = sGyroSource == 0 || sGyroSource == 2;
  float yaw = 0.f;
  float pitch = 0.f;
  bool haveRates = false;

  if (wantController) {
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      if (SDL_GamepadHasSensor(pad, SDL_SENSOR_GYRO)) {
        if (!sControllerGyroEnabled) {
          SDL_SetGamepadSensorEnabled(pad, SDL_SENSOR_GYRO, true);
          sControllerGyroEnabled = true;
        }
        float data[3];
        if (SDL_GetGamepadSensorData(pad, SDL_SENSOR_GYRO, data, 3)) {
          // Radians per second; x is pitch, y is yaw.
          yaw = data[1];
          pitch = data[0];
          haveRates = true;
          sGyroStatus = "controller";
        }
      }
    }
  }

  if (!haveRates && wantPhone) {
    if (sPhoneGyro == nullptr && !sPhoneGyroSearched) {
      sPhoneGyroSearched = true;
      int count = 0;
      if (SDL_SensorID* ids = SDL_GetSensors(&count)) {
        for (int i = 0; i < count; ++i) {
          if (SDL_GetSensorTypeForID(ids[i]) == SDL_SENSOR_GYRO) {
            sPhoneGyro = SDL_OpenSensor(ids[i]);
            break;
          }
        }
        SDL_free(ids);
      }
    }
    if (sPhoneGyro != nullptr) {
      float data[3];
      if (SDL_GetSensorData(sPhoneGyro, data, 3)) {
        yaw = data[1];
        pitch = data[0];
        haveRates = true;
        sGyroStatus = "phone";
      }
    }
  }

  if (!haveRates) {
    sGyroStatus = "no gyro found";
    return;
  }

  bool active = sGyroMode == 2;
  if (!active) {
    // Hold to aim: right stick click on a pad, left ctrl on a keyboard.
    if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
      active = SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    }
    if (!active) {
      const bool* keys = SDL_GetKeyboardState(nullptr);
      active = keys != nullptr && keys[SDL_SCANCODE_LCTRL] != 0;
    }
  }
  if (!active) {
    sGyroStatus = "held off";
    return;
  }

  const float dt = TickPeriod();
  if (!std::isfinite(dt) || dt <= 0.f) {
    return;
  }
  // x right / y up, the same shape AddStickAim takes; the aim state expects
  // right/down positive.
  sMouseFrameX += yaw * sGyroRate * dt;
  sMouseFrameY -= pitch * sGyroRate * dt;
}

void ResetMouseAim() {
  sMouseAimState.Reset();
  sMouseGameplayActive = false;
  sMouseButtonGate.Reset();
  sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
}

void SetMouseCaptured(bool captured) {
  sMouseCaptured = captured;
  if (!captured) {
    sMouseButtonGate.Reset();
    sMousePendingX = sMousePendingY = sMouseFrameX = sMouseFrameY = 0.f;
  }
}

bool MouseCaptured() { return sMouseCaptured; }
bool MouseGameplayActive() { return sMouseGameplayActive; }
void SetMouseGameplayActive(bool active) {
  sMouseGameplayActive = active;
  if (!active) ResetMouseAim();
}
bool MouseInvertX() { EnsureInitialized(); return sMouseInvertX; }
bool MouseInvertY() { EnsureInitialized(); return sMouseInvertY; }
bool MouseButtons() { EnsureInitialized(); return sMouseButtons; }
bool MouseCrosshair() { EnsureInitialized(); return sMouseCrosshair; }
unsigned MouseWeaponButtons(unsigned held) {
  return sMouseButtonGate.Poll(MouseAim() && MouseButtons() && MouseGameplayActive() &&
                               MouseCaptured() && !Visible(), held);
}
void NoteMouseButton(bool synthetic, unsigned mask, bool down) {
  sMouseHeldButtons.Note(synthetic, mask, down);
}
void ClearMouseButtons() { sMouseHeldButtons.Clear(); }
unsigned MouseHeldButtons() { return sMouseHeldButtons.Held(); }

bool UpdateMouseAim(bool active, bool locked, float x, float y, float z) {
  SetMouseGameplayActive(active);
  const bool applied = sMouseAimState.Update(active, locked, x, y, z, sMouseFrameX, sMouseFrameY,
                                            MouseSensitivity(), MouseInvertX(), MouseInvertY());
  if (applied) sMouseFrameX = sMouseFrameY = 0.f;
  return applied;
}
void SynchronizeMouseAim(float x, float y, float z) { sMouseAimState.Synchronize(x, y, z); }

float MouseSensitivity() {
  EnsureInitialized();
  return sMouseSensitivity;
}

void SetMouseSensitivity(float radiansPerPixel) {
  EnsureInitialized();
  if (std::isfinite(radiansPerPixel) && radiansPerPixel > 0.f) {
    sMouseSensitivity = radiansPerPixel;
  }
}

void AddMouseDelta(float dx, float dy) {
  if (!sMouseCaptured || !MouseAim() || Visible() || !std::isfinite(dx) || !std::isfinite(dy)) {
    return;
  }
  sMousePendingX += dx;
  sMousePendingY += dy;
}

void BeginFrameMouse() {
  sMouseFrameX = sMousePendingX;
  sMouseFrameY = sMousePendingY;
  sMousePendingX = 0.f;
  sMousePendingY = 0.f;
}

void GetFrameMouseDelta(float& dx, float& dy) {
  dx = sMouseFrameX;
  dy = sMouseFrameY;
}

float AimYaw() { return sMouseAimState.yaw; }
float AimPitch() { return sMouseAimState.pitch; }
bool AimInitialized() { return sMouseAimState.initialized; }

bool AiAudioEnabled() {
  EnsureInitialized();
  return AIPortOutputEnabled() != 0;
}

void SetAiAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sAiAudioEnabled == enabled && AiAudioEnabled() == enabled) {
    return;
  }
  sAiAudioEnabled = enabled;
  AIPortSetOutputEnabled(enabled ? 1 : 0);
}

bool MusyxAudioEnabled() {
  EnsureInitialized();
  return sMusyxAudioEnabled;
}

void SetMusyxAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sMusyxAudioEnabled == enabled) {
    return;
  }
  sMusyxAudioEnabled = enabled;
  salSetMuted(enabled ? 0 : 1);
}

void RequestReset() { sResetRequested = true; }

bool ConsumeResetRequest() {
  const bool requested = sResetRequested;
  sResetRequested = false;
  return requested;
}

void SetStateManager(CStateManager* mgr) { sStateManager = mgr; }
CStateManager* StateManager() { return sStateManager; }
void RequestTeleport(int areaId) { sPendingTeleport = areaId; }
bool ConsumeTeleportRequest(int& areaId) {
  if (sPendingTeleport < 0) {
    return false;
  }
  areaId = sPendingTeleport;
  sPendingTeleport = -1;
  return true;
}
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId) {
  sWorldTeleportWorld = worldId;
  sWorldTeleportArea = areaAssetId;
  sHasWorldTeleport = true;
}
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId) {
  if (!sHasWorldTeleport) {
    return false;
  }
  worldId = sWorldTeleportWorld;
  areaAssetId = sWorldTeleportArea;
  sHasWorldTeleport = false;
  return true;
}

void RequestWorldSweep() {
  if (sWorldSweepRequested || sWorldSweep.active || sHasWorldTeleport || sPendingTeleport >= 0)
    return;
  sWorldSweepRequested = true;
}

// Activates one layer of the current area and deactivates the rest, so the
// area's objects are built for that layer when it is next reconstructed.
// Layer 0 is the one a fresh save has, which is why a plain tour never sees
// anything behind another layer.
void SetSweepLayer(CStateManager& mgr, CWorld* world, TAreaId area, int layer) {
  rstl::rc_ptr< CScriptLayerManager >& layers = mgr.WorldLayerState();
  if (layers.IsNull())
    return;
  const int count = layers->GetAreaLayerCount(area);
  for (int i = 0; i < count; ++i)
    layers->SetLayerActive(area, TLayerId(i), i == layer);
  (void)world;
}

bool SweepLayersEnabled() {
  const char* value = std::getenv("MP_RANDO_SWEEP_LAYERS");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

bool ConsumeWorldSweepRequest(CStateManager& mgr) {
  // This entry point is called only by gameplay, never the frontend or UI.
  static const bool envChecked = [] {
    const char* value = std::getenv("MP_RANDO_SWEEP");
    if (value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0)
      RequestWorldSweep();
    return true;
  }();
  (void)envChecked;
  WorldSweep& sweep = sWorldSweep;
  if ((!sWorldSweepRequested && !sweep.active) || mgr.GetWantsToQuit()) {
    return false;
  }
  if (sHasWorldTeleport || sPendingTeleport >= 0 || sResetRequested) {
    // A manual debug request wins; do not resume the tour behind the user's back.
    if (sweep.active) std::fputs("[sweep] cancelled: another debug teleport/reset\n", stderr);
    sweep = {};
    sWorldSweepRequested = false;
    return false;
  }
  CWorld* world = mgr.World();
  if (mgr.GetGameState() != CStateManager::kGS_Running || world == nullptr ||
      gpGameState == nullptr || gpMemoryCard == nullptr) return false;

  if (sWorldSweepRequested) {
    const auto& worlds = gpMemoryCard->GetMemoryWorlds();
    if (worlds.empty()) return false;
    sweep = {};
    // MP_RANDO_SWEEP_WORLDS=<hex id>[,<hex id>...] limits the tour to those
    // worlds, e.g. to redo one world without walking the seven before it.
    std::vector< uint32_t > only;
    if (const char* list = std::getenv("MP_RANDO_SWEEP_WORLDS")) {
      for (const char* p = list; *p != '\0';) {
        char* end = nullptr;
        const unsigned long id = std::strtoul(p, &end, 16);
        if (end == p) {
          ++p;
          continue;
        }
        only.push_back(static_cast< uint32_t >(id));
        p = end;
      }
    }
    const auto wanted = [&only](uint32_t id) {
      if (only.empty()) return true;
      for (uint32_t w : only)
        if (w == id) return true;
      return false;
    };
    // Visit the live world first so its MLVL can supply the first area list;
    // later worlds are entered at area zero, which also visits their first area.
    if (wanted(world->IGetWorldAssetId())) sweep.worlds.push_back(world->IGetWorldAssetId());
    for (const auto& entry : worlds) {
      if (entry.first != world->IGetWorldAssetId() && wanted(entry.first))
        sweep.worlds.push_back(entry.first);
    }
    sWorldSweepRequested = false;
    if (sweep.worlds.empty()) {
      std::fputs("[sweep] stopped: MP_RANDO_SWEEP_WORLDS matches no world\n", stderr);
      sweep = {};
      return false;
    }
    sweep.active = true;
    std::fprintf(stderr, "[sweep] begin: %zu worlds (use MP_RANDO_DUMP=1 for pickups)\n",
                 sweep.worlds.size());
    if (world->IGetWorldAssetId() != sweep.worlds[0]) {
      // The live world was filtered out: enter the first wanted world the same
      // way a finished world hands over to the next one.
      sweep.waiting = true;
      RequestWorldTeleport(sweep.worlds[0], 0u);
      return true;
    }
  }
  if (world->IGetWorldAssetId() != sweep.worlds[sweep.world]) {
    std::fputs("[sweep] cancelled: gameplay changed worlds\n", stderr);
    sweep = {};
    return false;
  }
  if (sweep.areas.empty()) {
    for (int i = 0; i < world->IGetAreaCount(); ++i)
      sweep.areas.push_back(world->IGetAreaAlways(TAreaId(i))->IGetAreaAssetId());
    if (sweep.areas.empty()) {
      std::fputs("[sweep] stopped: world has no areas\n", stderr);
      sweep = {};
      return false;
    }
    std::fprintf(stderr, "[sweep] world %zu/%zu: %08X, %zu areas\n", sweep.world + 1,
                 sweep.worlds.size(), sweep.worlds[sweep.world], sweep.areas.size());
    if (SweepLayersEnabled()) {
      // How many layers the widest area has, so every area gets a pass per
      // layer even where its own count is lower. An area with fewer layers
      // simply rebuilds the same thing, which costs a restart and nothing else.
      int widest = 1;
      for (int i = 0; i < world->IGetAreaCount(); ++i) {
        const int count = mgr.WorldLayerState()
                              ? mgr.WorldLayerState()->GetAreaLayerCount(TAreaId(i))
                              : 1;
        if (count > widest)
          widest = count;
      }
      sweep.layerCount = widest;
      std::fprintf(stderr, "[sweep] layers: up to %d per area\n", widest);
    }
  }

  // Count consecutive quiet simulation ticks, not rendered frames or a wall
  // clock timeout. Current-area construction alone does not imply that adjacent
  // areas, map tiles and factory requests have finished streaming.
  const TAreaId current = world->GetCurrentAreaId();
  const bool areaExists = world->DoesAreaExist(current);
  const bool areaValid = areaExists && world->GetArea(current)->IsValidated();
  const bool loadingIdle = world->GetChainHead(CWorld::kC_Loading) == CWorld::GetAliveAreasEnd();
  const bool freeingIdle = world->GetChainHead(CWorld::kC_ToDeallocate) == CWorld::GetAliveAreasEnd();
  const bool mapIdle = !world->GetMapWorld()->IsMapAreasStreaming();
  const bool loadsIdle = !gpResourceFactory->HasPendingLoads();
  const bool ready = areaExists && areaValid && loadingIdle && freeingIdle && mapIdle && loadsIdle;
  if (!ready) {
    // A tour that never settles is otherwise invisible: no output, no
    // progress, just frames. Say what is still outstanding, and how long it has
    // been that way, so the stall names itself instead of looking like work.
    if (sweep.settledTicks == 0) {
      std::fprintf(stderr,
                   "[sweep] waiting on area %d: %s%s%s%s%s (target %08X, pass %zu/%zu)\n",
                   static_cast<int>(current.Value()), areaExists ? "" : "no-area ",
                   areaValid ? "" : "unvalidated ", loadingIdle ? "" : "loading ",
                   freeingIdle ? "" : "freeing ", mapIdle ? "" : "map-streaming ",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area], sweep.area + 1,
                   sweep.areas.size());
    }
    // 10 seconds of simulation. A pass that has not settled by then is not
    // going to: the loading chain stays put while the area is alive and
    // nothing else moves it. The window is short because the wait is measured
    // in ticks and a throttled run reaches a tick slowly.
    if (++sweep.stalledTicks == 600) {
      // One area that will not settle must not end a whole tour: the rest of
      // the world is still worth dumping, and this area's objects were built on
      // the way in even if the settle check never agreed. Report it, skip the
      // pass, and carry on from the next one.
      std::fprintf(stderr, "[sweep] giving up on %08X after 10s: %s%s%s%s%s\n",
                   sweep.areas.empty() ? 0u : sweep.areas[sweep.area],
                   areaExists ? "" : "no-area ", areaValid ? "" : "unvalidated ",
                   loadingIdle ? "" : "loading ", freeingIdle ? "" : "freeing ",
                   mapIdle ? "" : "map-streaming ");
      sweep.stalledTicks = 0;
      sweep.settledTicks = 0;
      sweep.revisiting = false;
      if (sweep.waiting) {
        ++sweep.completedAreas;
        ++sweep.area;
        sweep.layer = 0;
        if (sweep.area >= sweep.areas.size()) {
          sweep.areas.clear();
          sweep.area = 0;
          ++sweep.world;
        }
        sweep.waiting = false;
      }
      if (sweep.area >= sweep.areas.size() || sweep.world >= sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      return true;
    }
    sweep.settledTicks = 0;
    return false;
  }
  sweep.stalledTicks = 0;
  if (++sweep.settledTicks < 30) return false;
  sweep.settledTicks = 0;
  if (sweep.waiting) {
    const uint32_t settled = world->IGetAreaAlways(current)->IGetAreaAssetId();
    if (sweep.revisiting) {
      // This is the hop away from the area being re-layered: its objects have
      // been dumped, so all that matters is that it is gone by the time we go
      // back. Wait here, then return to it with the new layer active.
      sweep.revisiting = false;
      std::fprintf(stderr, "[sweep] left %08X for layer %d; returning\n", settled,
                   sweep.layer + 1);
      RequestWorldTeleport(sweep.worlds[sweep.world], sweep.areas[sweep.area]);
      return true;
    }
    if (settled != sweep.areas[sweep.area]) {
      // The destination's own scripts moved the player on (Impact Crater's
      // spawn points do). Its objects, and so its dump, were already built.
      std::fprintf(stderr, "[sweep] note: %08X moved the player to %08X; continuing\n",
                   sweep.areas[sweep.area], settled);
    }
    // One area, one layer: the count reports passes, not distinct areas, so a
    // multi-layered area is visibly a few.
    ++sweep.completedAreas;
    std::fprintf(stderr, "[sweep] area %zu/%zu: %08X layer %d/%d (total %u)\n", sweep.area + 1,
                 sweep.areas.size(), sweep.areas[sweep.area], sweep.layer + 1, sweep.layerCount,
                 sweep.completedAreas);
    if (++sweep.layer < sweep.layerCount) {
      // Another layer of the area just built. The layer state has to be set
      // before the area is reconstructed, because CGameArea builds the objects
      // of the layers that are active at construction time - which is why a
      // pickup behind an inactive layer is not in the dump at all.
      ++sweep.layerPasses;
      SetSweepLayer(mgr, world, current, sweep.layer);
      std::fprintf(stderr, "[sweep] re-entering %08X with layer %d active\n",
                   sweep.areas[sweep.area], sweep.layer);
      // The area has to be unloaded before it is rebuilt: SetLayerActive is a
      // bit flip, and travelling straight back onto a live area re-enters it
      // without ever scheduling its load again, so the pass never settles. Two
      // hops: out to the world's first area, which unloads this one, and the
      // return below lands on it with the new layer active.
      sweep.revisiting = true;
      RequestWorldTeleport(sweep.worlds[sweep.world], 0u);
      return true;
    }
    sweep.layer = 0;
    if (++sweep.area == sweep.areas.size()) {
      if (++sweep.world == sweep.worlds.size()) {
        std::fprintf(stderr, "[sweep] complete: %zu worlds, %u areas\n", sweep.worlds.size(),
                     sweep.completedAreas);
        sweep = {};
        return false;
      }
      sweep.areas.clear();
      sweep.area = 0;
    }
  }
  // QuitGame is consumed in this same tick. The next call with !GetWantsToQuit
  // belongs to the new manager even if the allocator reused its old address.
  sweep.waiting = true;
  RequestWorldTeleport(sweep.worlds[sweep.world],
                       sweep.areas.empty() ? 0u : sweep.areas[sweep.area]);
  return true;
}

bool Visible() {
  EnsureInitialized();
  return sVisible;
}

bool OverlayVisible() { return sOverlayVisible.load(std::memory_order_acquire); }

bool TwinStickFlag() { return sTwinStickFlag.load(std::memory_order_acquire); }

void SaveSettingsNow() {
  EnsureInitialized();
  SaveSettings();
}

// The overlay was laid out for a mouse. On a touchscreen it gets a full-screen
// panel with a page list instead of tabs, bigger targets and drag scrolling.
// MP_TOUCH_UI forces that layout on the desktop, to try it without a phone.
bool TouchUi() {
#if defined(__ANDROID__)
  return true;
#else
  static const bool sTouchUi = std::getenv("MP_TOUCH_UI") != nullptr;
  return sTouchUi;
#endif
}

SDL_Window* MainWindow() {
  static SDL_Window* sWindow = nullptr;
  if (sWindow == nullptr) {
    int windowCount = 0;
    if (SDL_Window** windows = SDL_GetWindows(&windowCount)) {
      if (windowCount > 0) {
        sWindow = windows[0];
      }
      SDL_free(windows);
    }
  }
  return sWindow;
}

bool WindowSize(int& width, int& height) {
  SDL_Window* window = MainWindow();
  return window != nullptr && SDL_GetWindowSize(window, &width, &height) && width > 0 &&
         height > 0;
}

// Sizes in unscaled pixels; UpdateUiScale multiplies them by the display scale
// like the defaults. A fingertip covers far more than a cursor does, so frames,
// grabs and scrollbars grow, and TouchExtraPadding widens every hit box a
// little beyond what is drawn.
void ApplyTouchStyle(ImGuiStyle& style) {
  style.WindowPadding = ImVec2(10.f, 10.f);
  style.FramePadding = ImVec2(10.f, 7.f);
  style.ItemSpacing = ImVec2(10.f, 8.f);
  style.ItemInnerSpacing = ImVec2(8.f, 6.f);
  style.CellPadding = ImVec2(6.f, 4.f);
  style.TouchExtraPadding = ImVec2(3.f, 3.f);
  style.IndentSpacing = 22.f;
  style.ScrollbarSize = 18.f;
  style.GrabMinSize = 18.f;
  style.FrameRounding = 5.f;
  style.GrabRounding = 5.f;
  style.ScrollbarRounding = 9.f;
  style.TabRounding = 5.f;
}

// Phones report a density of roughly 3, which leaves ImGui's default 13px font
// unreadably small, so scale the overlay to match the display. The scale is not
// known on the first frame, so keep watching for it instead of latching once.
void UpdateUiScale() {
  if (ImGui::GetCurrentContext() == nullptr) {
    return;
  }
  static bool sInitialized = false;
  static float sAppliedScale = 1.f;
  if (!sInitialized) {
    sInitialized = true;
    // The overlay has to size itself to the scaled font, so do not restore a
    // window size remembered from a previous, smaller run.
    ImGui::GetIO().IniFilename = nullptr;
    // Before any scaling, so the scaling applies to these sizes too.
    if (TouchUi()) {
      ApplyTouchStyle(ImGui::GetStyle());
    }
  }
  SDL_Window* window = MainWindow();
  if (window == nullptr) {
    return;
  }
  const float displayScale = SDL_GetWindowDisplayScale(window);
  const float uiScale = std::clamp(displayScale, 1.f, 4.f);
  if (uiScale == sAppliedScale) {
    return;
  }
  const float ratio = uiScale / sAppliedScale;
  sAppliedScale = uiScale;
  ImGui::GetStyle().ScaleAllSizes(ratio);
  ImGui::GetIO().FontGlobalScale *= ratio;
}

void RequestToggle() { sToggleRequested.store(true, std::memory_order_release); }

void UpdateControllerNav() {
  EnsureInitialized();
  // Registered here rather than in EnsureInitialized so the event system is only
  // touched from the game thread; the Java visibility query can reach that
  // initialization from the UI thread.
  static bool sEventWatchRegistered = false;
  if (!sEventWatchRegistered) {
    sEventWatchRegistered = true;
    SDL_AddEventWatch(debug_event_watch, nullptr);
  }
  UpdateUiScale();
  if (sToggleRequested.exchange(false, std::memory_order_acq_rel)) {
    Toggle();
  }
  sOverlayVisible.store(sVisible, std::memory_order_release);
  sTwinStickFlag.store(sTwinStick, std::memory_order_release);

  ImGuiIO& io = ImGui::GetIO();
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  // The SDL3 backend calls SDL_ShowCursor on every NewFrame, and the main loop
  // hides the cursor again during play, so it flickered wherever relative
  // mouse mode wasn't hiding it (Android, menus, cutscenes). Let the backend
  // own the cursor only while the overlay is open.
  if (sVisible) {
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
  } else {
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  }

  SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
  if (pad == nullptr) {
    return;
  }
  // While the Controls tab captures a pad input, the pad binds instead of
  // navigating (releasing everything here also ends any nav press in progress).
  const bool capturing = PortControls::Capturing();
  const auto held = [pad, capturing](SDL_GamepadButton button) {
    return !capturing && SDL_GetGamepadButton(pad, button);
  };
  const auto axis = [pad, capturing](SDL_GamepadAxis a) {
    return capturing ? Sint16{0} : SDL_GetGamepadAxis(pad, a);
  };
  constexpr Sint16 kStickThreshold = 16000;
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                 held(SDL_GAMEPAD_BUTTON_DPAD_UP) || axis(SDL_GAMEPAD_AXIS_LEFTY) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                 held(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || axis(SDL_GAMEPAD_AXIS_LEFTY) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                 held(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || axis(SDL_GAMEPAD_AXIS_LEFTX) < -kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                 held(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || axis(SDL_GAMEPAD_AXIS_LEFTX) > kStickThreshold);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, held(SDL_GAMEPAD_BUTTON_SOUTH));
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, held(SDL_GAMEPAD_BUTTON_EAST));
  io.AddKeyEvent(ImGuiKey_GamepadL1, held(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
  io.AddKeyEvent(ImGuiKey_GamepadR1, held(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER));

  static bool sChordHeld = false;
  const bool chord = held(SDL_GAMEPAD_BUTTON_START) && held(SDL_GAMEPAD_BUTTON_BACK);
  if (chord && !sChordHeld) {
    Toggle();
  }
  sChordHeld = chord;
}

void Toggle() {
  EnsureInitialized();
  sVisible = !sVisible;
  SetMouseCaptured(false);
}

void DrawPerformanceTab() {
  ImGui::Text("Build: %s", MP_BUILD_REVISION);
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("60 FPS cap (target)", &frameLimit)) {
    SetFrameLimitEnabled(frameLimit);
    MarkDirty();
  }
  // Both, because they answer different questions. Throughput is what the
  // machine produces once the pacing wait is excluded; presented is what
  // reaches the screen. A player seeing stutter wants the second one, and the
  // gap between them is the headroom.
  ImGui::Text("Presented: %.1f FPS", sActualFps);
  ImGui::Text("Throughput: %.1f FPS (headroom at the current frame cost)", sThroughputFps);
  if (sSimAdaptive) {
    ImGui::Text("Measured simulation: %.1f ticks/s (adaptive)", sActualTps);
  } else {
    ImGui::Text("Measured simulation: %.1f ticks/s (target %u)", sActualTps, sSimRate);
  }
  ImGui::Text("Frame time: %.2f ms", static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);

  ImGui::Separator();
  ImGui::TextUnformatted("Experimental: simulation rate");
  bool adaptive = sSimAdaptive;
  if (ImGui::Checkbox("Adaptive (follow frame rate)", &adaptive)) {
    PortDebug::SetSimAdaptive(adaptive);
  }
  ImGui::BeginDisabled(adaptive);
  int simRate = static_cast< int >(sSimRate);
  if (ImGui::SliderInt("Sim Hz", &simRate, 30, 480)) {
    PortDebug::SetSimRate(static_cast< unsigned >(simRate));
  }
  ImGui::EndDisabled();
  if (adaptive) {
    ImGui::TextWrapped(
        "One step per frame with dt = the measured frame time (clamped 30-480 Hz), "
        "so a variable frame rate is matched exactly. Leave the FPS cap off.");
  } else if (simRate != 60) {
    ImGui::TextWrapped(
        "60 Hz is console-accurate. Higher values step the game logic at the display "
        "rate instead of interpolating the camera; leave the FPS cap off for it to "
        "matter.");
  }
}

void DrawCutscenesTab() {
  if (ImGui::Checkbox("Skip / fast-forward cutscenes", &sSkipCutscenes)) {
    MarkDirty();
  }
  if (ImGui::SliderFloat("Cutscene speed", &sCutsceneSpeed, 1.f, 32.f, "%.0fx")) {
    MarkDirty();
  }
  if (ImGui::Button("Reset cutscene speed")) {
    sCutsceneSpeed = 8.f;
    MarkDirty();
  }
}

void DrawRenderTab() {
  bool vsync = sVsyncEnabled;
  if (ImGui::Checkbox("Vsync", &vsync)) {
    SetVsyncEnabled(vsync);
    MarkDirty();
  }

  int aspect = static_cast< int >(sAspectMode);
  if (ImGui::Combo("Aspect ratio", &aspect, "4:3\0" "16:9\0" "Follow window\0")) {
    SetAspectMode(static_cast< EAspectMode >(aspect));
    MarkDirty();
  }

  bool hudWide = sHudWide;
  if (ImGui::Checkbox("Widescreen HUD (spread to edges)", &hudWide)) {
    SetHudWide(hudWide);
    MarkDirty();
  }
  ImGui::TextWrapped(
      "Keeps each HUD element's shape but spreads its position so edge elements "
      "reach the wide corners. Only affects the in-game HUD, not menus.");

  bool autoScale = sRenderScale <= 0.f;
  if (ImGui::Checkbox("Auto render scale (native)", &autoScale)) {
    SetRenderScale(autoScale ? 0.f : 1.f);
    MarkDirty();
  }
  if (!autoScale) {
    float scale = sRenderScale;
    if (ImGui::SliderFloat("EFB scale", &scale, 1.f, 2.f, "%.2fx")) {
      SetRenderScale(scale);
      MarkDirty();
    }
    ImGui::TextUnformatted("Scales the internal EFB; higher values use more GPU memory.");
  }

  ImGui::Text("HD texture set: %s", PortTextures::DeviceName());
  ImGui::TextWrapped(
      "Selected from the connected controller (xbox, playstation, switch, "
      "gamecube, standard, keyboard); set MP_TEXTURE_DEVICE to override.");
}

void DrawInputTab() {
  bool mouseAim = sMouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    MarkDirty();
  }
  bool twinStick = sTwinStick;
  if (ImGui::Checkbox("Twin stick (right stick aims)", &twinStick)) {
    SetTwinStick(twinStick);
    MarkDirty();
  }
  ImGui::BeginDisabled(!sTwinStick);
  float stickRate = sStickAimRate;
  if (ImGui::SliderFloat("Stick aim speed", &stickRate, 100.f, 3000.f, "%.0f px/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetStickAimRate(stickRate);
  }
  ImGui::EndDisabled();
  ImGui::TextWrapped(
      "Twin stick uses the right stick as a direct camera aim (the same path as "
      "the mouse) and consumes it, so it no longer free-looks. Fire stays on "
      "whatever is bound to A; remap it in the Controls tab.");
  ImGui::SeparatorText("Gyro aim");
  const char* gyroModes[] = {"Off", "Hold to aim", "Always aim"};
  int gyroMode = sGyroMode;
  if (ImGui::Combo("Mode", &gyroMode, gyroModes, 3)) {
    SetGyroMode(gyroMode);
  }
  ImGui::BeginDisabled(sGyroMode == 0);
  const char* gyroSources[] = {"Auto", "Controller", "Phone"};
  int gyroSource = sGyroSource;
  if (ImGui::Combo("Source", &gyroSource, gyroSources, 3)) {
    SetGyroSource(gyroSource);
  }
  float gyroRate = sGyroRate;
  if (ImGui::SliderFloat("Sensitivity", &gyroRate, 50.f, 3000.f, "%.0f px/s per rad/s",
                         ImGuiSliderFlags_Logarithmic)) {
    SetGyroRate(gyroRate);
  }
  ImGui::Text("Gyro: %s", GyroStatus());
  ImGui::TextWrapped(
      "Tilt the pad or the phone to aim. Hold to aim uses right stick click or "
      "left ctrl. Needs mouse aim or twin stick, since the gyro feeds that same "
      "aim.");
  ImGui::EndDisabled();

  if (ImGui::Checkbox("Invert mouse X", &sMouseInvertX)) {
    MarkDirty();
  }
  if (ImGui::Checkbox("Invert mouse Y", &sMouseInvertY)) {
    MarkDirty();
  }
  if (ImGui::Checkbox("Mouse weapon buttons", &sMouseButtons)) {
    sMouseButtonGate.Reset();
    MarkDirty();
  }
  if (ImGui::Checkbox("Mouse-aim crosshair", &sMouseCrosshair)) {
    MarkDirty();
  }
  ImGui::TextUnformatted("Left: fire/charge   Right: lock-on   Middle: missile");
  ImGui::TextUnformatted("Existing keyboard/controller weapon bindings also work.");
  if (ImGui::SliderFloat("Sensitivity", &sMouseSensitivity, 0.0005f, 0.02f, "%.4f rad/px",
                         ImGuiSliderFlags_Logarithmic)) {
    MarkDirty();
  }
}

void DrawAudioTab() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
    MarkDirty();
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
    MarkDirty();
  }
}

void DrawVoicesTab() {  PortMusyxVoice voices[64];
  const int count = MusyxPortCopyVoices(voices, 64);
  if (count == 0) {
    ImGui::TextUnformatted("No active MusyX voices.");
    return;
  }

  struct Agg {
    PortMusyxVoice voice;
    int instances;
  };
  std::vector< Agg > aggs;
  for (int i = 0; i < count; ++i) {
    bool found = false;
    for (Agg& agg : aggs) {
      if (agg.voice.smpId == voices[i].smpId) {
        ++agg.instances;
        if (voices[i].rms > agg.voice.rms) {
          agg.voice = voices[i];
        }
        found = true;
        break;
      }
    }
    if (!found) {
      aggs.push_back(Agg{voices[i], 1});
    }
  }
  std::sort(aggs.begin(), aggs.end(),
            [](const Agg& a, const Agg& b) { return a.voice.rms > b.voice.rms; });

  ImGui::TextUnformatted("Active samples, loudest first. Mute one to isolate it.");
  for (const Agg& agg : aggs) {
    ImGui::PushID(static_cast< int >(agg.voice.smpId));
    bool muted = MusyxPortIsSampleMuted(agg.voice.smpId) != 0;
    if (ImGui::Checkbox("##mute", &muted)) {
      MusyxPortSetSampleMuted(agg.voice.smpId, muted ? 1 : 0);
      MarkDirty();
    }
    ImGui::SameLine();
    ImGui::Text("smp %u  %s  len %u  pitch %u  rms %d  vol %u/%u  x%d", agg.voice.smpId,
                agg.voice.looped ? "loop" : "one-shot", agg.voice.length, agg.voice.pitch,
                agg.voice.rms, agg.voice.volL, agg.voice.volR, agg.instances);
    ImGui::PopID();
  }
  if (ImGui::Button("Unmute all")) {
    MusyxPortClearSampleMutes();
    MarkDirty();
  }
}

void DrawSessionTab() {
  if (ImGui::Button("Restart to menu")) {
    RequestReset();
  }
  ImGui::SameLine();
  if (ImGui::Button("Screenshot (F12)")) {
    aurora::request_screenshot();
  }
  ImGui::Separator();
  ImGui::TextUnformatted("Settings are saved automatically when changed.");
  const std::string path = SettingsFilePath();
  ImGui::TextWrapped("File: %s", path.c_str());
  if (ImGui::Button("Save settings now")) {
    sSettingsDirty = true;
    SaveSettings();
  }
  ImGui::SameLine();
  ImGui::TextUnformatted(sSettingsDirty ? "Unsaved changes" : "Saved");

  if (PortAp::Enabled()) {
    ImGui::SeparatorText("Archipelago");
    ImGui::TextWrapped("Status: %s", PortAp::StatusText());
    const char* seedName = PortAp::SeedName();
    if (seedName != nullptr && seedName[0] != '\0')
      ImGui::TextWrapped("Seed: %s", seedName);
    ImGui::Text("Items received: %d", PortAp::ItemCount());
    ImGui::Text("Location checks sent: %d", PortAp::CheckCount());
    ImGui::Text("Connection: %s", PortAp::Connected() ? "connected" : "not connected");
    const char* lastMessage = PortAp::LastMessage();
    if (lastMessage != nullptr && lastMessage[0] != '\0')
      ImGui::TextWrapped("Last message: %s", lastMessage);

    // Item tracker: the session's receipts, so an item that arrived while the
    // player was not looking at the HUD is still readable here.
    const std::vector< PortAp::TrackedItem > tracked = PortAp::TrackedItems();
    ImGui::SeparatorText("Received items");
    if (tracked.empty()) {
      ImGui::TextDisabled("Nothing yet.");
    } else {
      // Half the space the section has left, so the table does not push the
      // settings below it off the tab.
      if (ImGui::BeginTable("apTracked", 3,
                            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_BordersInnerH,
                            ImVec2(0.0f, ImGui::GetContentRegionAvail().y * 0.5f))) {
        ImGui::TableSetupColumn("Item");
        ImGui::TableSetupColumn("From");
        ImGui::TableSetupColumn("Step");
        ImGui::TableHeadersRow();
        for (const PortAp::TrackedItem& item : tracked) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.name.c_str());
          ImGui::TableNextColumn();
          ImGui::TextUnformatted(item.from.empty() ? "(your item)" : item.from.c_str());
          ImGui::TableNextColumn();
          if (item.total > 1) {
            ImGui::Text("%d of %d", item.step, item.total);
          } else {
            ImGui::TextDisabled("-");
          }
        }
        ImGui::EndTable();
      }
    }
  }
}

void GrantItem(CPlayerState& ps, CPlayerState::EItemType type, int amount, int capacity) {
  ps.SetPowerUp(type, capacity);
  ps.SetPickup(type, amount);
}

void DrawDebugTab() {
  CStateManager* mgr = sStateManager;
  if (mgr == nullptr) {
    ImGui::TextUnformatted("Waiting for gameplay...");
    return;
  }
  CPlayerState* ps = mgr->PlayerState();
  if (ps == nullptr) {
    ImGui::TextUnformatted("No player state.");
    return;
  }

  ImGui::Text("Health: %.0f / %.0f", ps->HealthInfo()->GetHP(), ps->CalculateHealth());
  if (ImGui::Button("Full health")) {
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }
  ImGui::SameLine();
  if (ImGui::Button("Grant everything")) {
    for (int i = CPlayerState::kIT_PowerBeam; i < CPlayerState::kIT_Max; ++i) {
      GrantItem(*ps, static_cast< CPlayerState::EItemType >(i), 1, 1);
    }
    GrantItem(*ps, CPlayerState::kIT_Missiles, 250, 250);
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, 8, 8);
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, 14, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Abilities");
  struct SItemToggle {
    const char* name;
    CPlayerState::EItemType type;
  };
  static const SItemToggle kItems[] = {
      {"Power Beam", CPlayerState::kIT_PowerBeam},
      {"Ice Beam", CPlayerState::kIT_IceBeam},
      {"Wave Beam", CPlayerState::kIT_WaveBeam},
      {"Plasma Beam", CPlayerState::kIT_PlasmaBeam},
      {"Charge Beam", CPlayerState::kIT_ChargeBeam},
      {"Super Missile", CPlayerState::kIT_SuperMissile},
      {"Ice Spreader", CPlayerState::kIT_IceSpreader},
      {"Wavebuster", CPlayerState::kIT_Wavebuster},
      {"Flamethrower", CPlayerState::kIT_Flamethrower},
      {"Combat Visor", CPlayerState::kIT_CombatVisor},
      {"Scan Visor", CPlayerState::kIT_ScanVisor},
      {"Thermal Visor", CPlayerState::kIT_ThermalVisor},
      {"X-Ray Visor", CPlayerState::kIT_XRayVisor},
      {"Morph Ball", CPlayerState::kIT_MorphBall},
      {"Morph Ball Bombs", CPlayerState::kIT_MorphBallBombs},
      {"Boost Ball", CPlayerState::kIT_BoostBall},
      {"Spider Ball", CPlayerState::kIT_SpiderBall},
      {"Space Jump Boots", CPlayerState::kIT_SpaceJumpBoots},
      {"Grapple Beam", CPlayerState::kIT_GrappleBeam},
      {"Gravity Suit", CPlayerState::kIT_GravitySuit},
      {"Varia Suit", CPlayerState::kIT_VariaSuit},
      {"Phazon Suit", CPlayerState::kIT_PhazonSuit},
  };
  for (const SItemToggle& item : kItems) {
    bool owned = ps->HasPowerUp(item.type);
    if (ImGui::Checkbox(item.name, &owned)) {
      if (owned) {
        GrantItem(*ps, item.type, 1, 1);
      } else {
        ps->SetPowerUp(item.type, 0);
        ps->SetPickup(item.type, 0);
      }
    }
  }

  int missiles = ps->GetItemAmount(CPlayerState::kIT_Missiles);
  if (ImGui::SliderInt("Missiles", &missiles, 0, 250)) {
    GrantItem(*ps, CPlayerState::kIT_Missiles, missiles, 250);
  }
  int powerBombs = ps->GetItemAmount(CPlayerState::kIT_PowerBombs);
  if (ImGui::SliderInt("Power Bombs", &powerBombs, 0, 8)) {
    GrantItem(*ps, CPlayerState::kIT_PowerBombs, powerBombs, 8);
  }
  int tanks = ps->GetItemAmount(CPlayerState::kIT_EnergyTanks);
  if (ImGui::SliderInt("Energy Tanks", &tanks, 0, 14)) {
    GrantItem(*ps, CPlayerState::kIT_EnergyTanks, tanks, 14);
    ps->HealthInfo()->SetHP(ps->CalculateHealth());
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Teleport");
  CWorld* world = mgr->World();
  if (world == nullptr) {
    ImGui::TextUnformatted("No world.");
    return;
  }
  if (mgr->GetGameState() != CStateManager::kGS_Running) {
    ImGui::TextUnformatted("(waiting for gameplay)");
  }
  const int areaCount = world->IGetAreaCount();
  const int current = world->GetCurrentAreaId().Value();
  ImGui::Text("Current area: %d of %d", current, areaCount);
  for (int i = 0; i < areaCount; ++i) {
    ImGui::PushID(i);
    if (ImGui::Button(i == current ? "Reload" : "Go")) {
      PortDebug::RequestTeleport(i);
    }
    ImGui::SameLine();
    ImGui::Text("Area %d", i);
    ImGui::PopID();
  }

  ImGui::Separator();
  ImGui::TextUnformatted("Worlds");
  if (gpMemoryCard == nullptr) {
    ImGui::TextUnformatted("(memory card not ready)");
    return;
  }
  static bool sWorldListBuilt = false;
  static std::vector< std::pair< uint32_t, std::string > > sWorldList;
  if (!sWorldListBuilt && !gpMemoryCard->GetMemoryWorlds().empty()) {
    sWorldListBuilt = true;
    const rstl::vector< CMemoryCard::MemoryWorld >& worlds = gpMemoryCard->GetMemoryWorlds();
    for (int i = 0; i < worlds.size(); ++i) {
      const uint32_t id = static_cast< uint32_t >(worlds[i].first);
      std::string name;
      const wchar_t* wide = worlds[i].second.GetFrontEndName();
      if (wide != nullptr) {
        for (const wchar_t* p = wide; *p != 0; ++p) {
          name.push_back(static_cast< char >(*p));
        }
      }
      if (name.empty()) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "MLVL %08X", static_cast< unsigned >(id));
        name = buf;
      }
      sWorldList.emplace_back(id, name);
    }
  }
  if (!sWorldListBuilt) {
    ImGui::TextUnformatted("(loading worlds...)");
    return;
  }
  for (const std::pair< uint32_t, std::string >& entry : sWorldList) {
    ImGui::PushID(static_cast< int >(entry.first));
    const bool isCurrent =
        gpGameState != nullptr && gpGameState->CurrentWorldAssetId() == entry.first;
    if (ImGui::Button(isCurrent ? "Here" : "Go")) {
      PortDebug::RequestWorldTeleport(entry.first, 0u);
    }
    ImGui::SameLine();
    ImGui::TextUnformatted(entry.second.c_str());
    ImGui::PopID();
  }
}

struct DebugPage {
  const char* name;
  void (*draw)();
};

const DebugPage kDebugPages[] = {
    {"Performance", DrawPerformanceTab}, {"Cutscenes", DrawCutscenesTab},
    {"Input", DrawInputTab},             {"Controls", PortControls::DrawTab},
    {"Render", DrawRenderTab},           {"Audio", DrawAudioTab},
    {"Voices", DrawVoicesTab},           {"Debug", DrawDebugTab},
    {"Session", DrawSessionTab},
};

// The innermost window under the finger that can actually scroll vertically,
// climbing out of child windows (a table, the page list) that cannot.
ImGuiWindow* ScrollableWindowAt(ImGuiWindow* window) {
  for (; window != nullptr; window = window->ParentWindow) {
    if (window->ScrollMax.y > 0.f && (window->Flags & ImGuiWindowFlags_NoScrollWithMouse) == 0) {
      return window;
    }
    if ((window->Flags & ImGuiWindowFlags_ChildWindow) == 0) {
      break;
    }
  }
  return nullptr;
}

bool IsResizeGrip(ImGuiWindow* window, ImGuiID id) {
  for (int n = 0; n < 4; ++n) {
    if (id == ImGui::GetWindowResizeCornerID(window, n) ||
        id == ImGui::GetWindowResizeBorderID(window, static_cast< ImGuiDir >(n))) {
      return true;
    }
  }
  return false;
}

// ImGui has no touch scrolling: a finger dragged down a page presses whatever
// it landed on and scrolls nothing. A mostly vertical drag is taken away from
// the widget it began on (so a button under it does not fire on release) and
// scrolls the window instead, and the finger's speed carries on as a fling
// after it lifts. A mostly horizontal drag is left alone, so sliders still
// work, and so are the scrollbar, a window being moved or resized, and drags
// that start outside the content area. Runs after NewFrame, before any window.
struct TouchScroll {
  ImGuiWindow* window = nullptr;
  bool decided = false;
  bool dragging = false;
  float velocity = 0.f; // pixels per second, positive scrolls down
};
TouchScroll sTouchScroll;

void UpdateTouchScroll() {
  ImGuiContext& g = *ImGui::GetCurrentContext();
  const ImGuiIO& io = g.IO;
  TouchScroll& scroll = sTouchScroll;
  const bool touch = io.MouseSource == ImGuiMouseSource_TouchScreen;

  if (io.MouseClicked[0]) {
    scroll = TouchScroll{};
    if (touch) {
      ImGuiWindow* window = ScrollableWindowAt(g.HoveredWindow);
      if (window != nullptr && window->InnerClipRect.Contains(io.MouseClickedPos[0])) {
        scroll.window = window;
      }
    }
  }
  if (scroll.window == nullptr) {
    return;
  }

  if (io.MouseDown[0]) {
    if (!scroll.decided) {
      const ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.f);
      // A share of the font size rather than io.MouseDragThreshold's fixed
      // pixels, which on a dense phone screen is a jitter, not a drag.
      const float threshold = g.FontSize * 0.6f;
      if (delta.x * delta.x + delta.y * delta.y < threshold * threshold) {
        return;
      }
      scroll.decided = true;
      ImGuiWindow* activeWindow = g.ActiveIdWindow;
      const bool ownDrag =
          g.MovingWindow != nullptr ||
          (g.ActiveId != 0 && activeWindow != nullptr &&
           (g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_X) ||
            g.ActiveId == ImGui::GetWindowScrollbarID(activeWindow, ImGuiAxis_Y) ||
            IsResizeGrip(activeWindow, g.ActiveId)));
      if (std::fabs(delta.y) <= std::fabs(delta.x) || ownDrag) {
        scroll.window = nullptr;
        return;
      }
      scroll.dragging = true;
    }
    if (!scroll.dragging) {
      return;
    }
    if (g.ActiveId != 0) {
      ImGui::ClearActiveID();
    }
    ImGui::SetScrollY(scroll.window, scroll.window->Scroll.y - io.MouseDelta.y);
    if (io.DeltaTime > 0.f) {
      // Smoothed, so the last jittery frame before the finger lifts does not
      // decide the whole fling.
      const float instant = -io.MouseDelta.y / io.DeltaTime;
      scroll.velocity += (instant - scroll.velocity) * 0.4f;
    }
    return;
  }

  // Released: keep scrolling at the finger's speed, easing off.
  if (!scroll.dragging) {
    scroll.window = nullptr;
    return;
  }
  const float y = scroll.window->Scroll.y;
  const bool atEdge = (scroll.velocity < 0.f && y <= 0.f) ||
                      (scroll.velocity > 0.f && y >= scroll.window->ScrollMax.y);
  if (atEdge || std::fabs(scroll.velocity) < g.FontSize) {
    scroll = TouchScroll{};
    return;
  }
  ImGui::SetScrollY(scroll.window, y + scroll.velocity * io.DeltaTime);
  scroll.velocity *= std::exp(-4.f * io.DeltaTime);
}

// A finger that lifts leaves ImGui's cursor where it was, so whatever was last
// tapped stays drawn as hovered. Move the cursor off-screen once the release
// has been seen; queued now, it lands on the next frame.
void ClearTouchHover() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.MouseSource == ImGuiMouseSource_TouchScreen && io.MouseReleased[0] &&
      !ImGui::IsAnyMouseDown()) {
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }
}

// Full screen inside the display's safe area (clear of the notch and the
// gesture bars), no title bar to drag, a Close button a thumb can hit, and a
// page list down the side in place of a tab strip too narrow to tap.
bool DrawTouchWindow() {
  const ImGuiViewport* viewport = ImGui::GetMainViewport();
  ImVec2 pos = viewport->WorkPos;
  ImVec2 size = viewport->WorkSize;
  SDL_Window* window = MainWindow();
  SDL_Rect safe;
  int windowWidth = 0, windowHeight = 0;
  if (window != nullptr && SDL_GetWindowSafeArea(window, &safe) &&
      SDL_GetWindowSize(window, &windowWidth, &windowHeight) && windowWidth > 0 &&
      windowHeight > 0 && safe.w > 0 && safe.h > 0) {
    // ImGui's display size need not be in window coordinates.
    const float sx = size.x / static_cast< float >(windowWidth);
    const float sy = size.y / static_cast< float >(windowHeight);
    pos = ImVec2(pos.x + safe.x * sx, pos.y + safe.y * sy);
    size = ImVec2(safe.w * sx, safe.h * sy);
  }
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(size, ImGuiCond_Always);
  constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoScrollbar |
                                      ImGuiWindowFlags_NoScrollWithMouse;
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port##touch", nullptr, kFlags)) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const char* const kClose = "Close";
    const float closeWidth = ImGui::CalcTextSize(kClose).x + style.FramePadding.x * 4.f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Metroid Prime Port");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - closeWidth);
    if (ImGui::Button(kClose, ImVec2(closeWidth, 0.f))) {
      open = false;
    }
    ImGui::Separator();

    static int sPage = 0;
    float listWidth = 0.f;
    for (const DebugPage& page : kDebugPages) {
      listWidth = std::max(listWidth, ImGui::CalcTextSize(page.name).x);
    }
    listWidth += style.FramePadding.x * 2.f + style.WindowPadding.x * 2.f;
    const float rowHeight = ImGui::GetFrameHeight() * 1.2f;
    if (ImGui::BeginChild("##pages", ImVec2(listWidth, 0.f), ImGuiChildFlags_Borders)) {
      for (int i = 0; i < static_cast< int >(ARRAY_SIZE(kDebugPages)); ++i) {
        if (ImGui::Selectable(kDebugPages[i].name, sPage == i, ImGuiSelectableFlags_None,
                              ImVec2(0.f, rowHeight))) {
          sPage = i;
        }
      }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    // Keyed by page, so each page keeps its own scroll position.
    ImGui::PushID(sPage);
    if (ImGui::BeginChild("##page", ImVec2(0.f, 0.f), ImGuiChildFlags_Borders)) {
      kDebugPages[sPage].draw();
    }
    ImGui::EndChild();
    ImGui::PopID();
  }
  ImGui::End();
  return open;
}

bool DrawDesktopWindow() {
  ImGui::SetNextWindowPos(ImVec2(8.f, 8.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.f, 200.f), ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port", &open, ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      ImGui::TextUnformatted("F1: hide   F10: frame limit   F12: screenshot");
      ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("##debug_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (const DebugPage& page : kDebugPages) {
        if (ImGui::BeginTabItem(page.name)) {
          page.draw();
          ImGui::EndTabItem();
        }
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();
  return open;
}

void DrawUI() {
  EnsureInitialized();
  if (!sAudioSettingsApplied) {
    // Apply persisted audio mutes once the backends are alive.
    sAudioSettingsApplied = true;
    SetAiAudioEnabled(sAiAudioEnabled);
    SetMusyxAudioEnabled(sMusyxAudioEnabled);
  }
  if (!sPresentationSettingsApplied) {
    // Apply persisted vsync once the swapchain surface exists (first drawn
    // frame), so the present mode is chosen from real surface capabilities.
    sPresentationSettingsApplied = true;
    aurora_enable_vsync(sVsyncEnabled && !sTurbo);
  }
  if (!sVisible) {
    sTouchScroll = TouchScroll{};
    return;
  }

  UpdateTouchScroll();
  const bool open = TouchUi() ? DrawTouchWindow() : DrawDesktopWindow();
  ClearTouchHover();

  if (!open) {
    sVisible = false;
  }

  if (sSettingsDirty) {
    SaveSettings();
  }
}

void LoadDiscPath() {
  const std::string path = SettingsFilePath();
  std::ifstream file(path);
  if (!file.is_open()) {
    return;
  }
  std::string line;
  while (std::getline(file, line)) {
    const size_t separator = line.find('=');
    if (separator == std::string::npos || Trim(line.substr(0, separator)) != "disc_path") {
      continue;
    }
    sDiscPath = Trim(line.substr(separator + 1));
    std::fprintf(stderr, "metroid_prime_port: saved disc image %s\n", sDiscPath.c_str());
    return;
  }
}

const char* DiscPath() {
  return sDiscPath.empty() ? nullptr : sDiscPath.c_str();
}

void SetDiscPath(const char* path) {
  sDiscPath = path != nullptr ? path : "";
  sSettingsDirty = true;
}

} // namespace PortDebug

#if defined(__ANDROID__)
// The touch overlay covers the display and consumes every touch before SDL
// sees it. While the debug overlay is open the game is paused and those touches
// belong to ImGui, so the Java side asks this and stops claiming them.
#if defined(__ANDROID__)
// The pad itself - descriptor, mapping, axis conversion, attach and detach -
// lives in platform/touch_pad.cpp so that it can be built and tested on the
// host. It used to be here, inside this #if, which meant it was never compiled
// anywhere except an Android build and no test could ever have caught a wrong
// button mapping. See tests/touch_pad.cpp.
namespace {
PortTouchPad::Pad g_touchPad;

PortTouchPad::Pad& TouchPad() {
  if (!g_touchPad.ok()) {
    g_touchPad = PortTouchPad::Attach();
    __android_log_print(ANDROID_LOG_INFO, "touchpad", "attached id=%d gamepad=%d open=%d",
                        g_touchPad.id, SDL_IsGamepad(g_touchPad.id) ? 1 : 0,
                        g_touchPad.ok() ? 1 : 0);
    if (!g_touchPad.ok())
      __android_log_print(ANDROID_LOG_ERROR, "touchpad", "attach failed: %s", SDL_GetError());
  }
  return g_touchPad;
}
} // namespace

// KNOWN LIMITATION: a short tap can be missed.
//
// The virtual joystick API is state-sampling, not event-queueing. Setting a
// button stores the latest value and marks it changed (SDL_virtualjoystick.c:401);
// the change is only delivered at the next update, which sends whatever the
// value is *then* (:742). So a press and release that both land between two
// updates leave only the release, and the game never sees the press. A quick tap
// on A or Start can therefore do nothing, most visibly while a game frame is
// stalled. Triggers behave the same way.
//
// This is not a data race - the setters hold SDL's joystick mutex, so nothing
// tears - and it is not specific to this port; it is how SDL's virtual joystick
// works. Sustained presses and ordinary releases are unaffected, which is why it
// has not shown up as "controls don't work".
//
// Fixing it properly means latching a press until the game has sampled it, and
// the latch has to be released on an update the port does not control. That is a
// real design problem, not a two-line patch, so it is recorded rather than
// half-solved. A missed tap is recoverable by tapping again; a control that fires
// when it should not is not.

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualButton(JNIEnv*, jclass, jint button,
                                                                 jboolean down) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualButton(pad, static_cast< int >(button), down == JNI_TRUE);
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeVirtualAxis(JNIEnv*, jclass, jint axis,
                                                               jfloat value) {
  if (SDL_Joystick* pad = TouchPad().handle) {
    SDL_SetJoystickVirtualAxis(pad, static_cast< int >(axis), PortTouchPad::AxisValue(value));
  }
}
#endif

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeDebugOverlayVisible(JNIEnv*, jclass) {
  return PortDebug::OverlayVisible() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeSetTouchDevice(JNIEnv*, jclass, jboolean xbox) {
  PortPrompts::NoteTouchInput(xbox == JNI_TRUE);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTwinStick(JNIEnv*, jclass) {
  return PortDebug::TwinStickFlag() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeToggleDebugOverlay(JNIEnv*, jclass) {
  PortDebug::RequestToggle();
}

// Whether a real pad, keyboard or mouse was used since the last call.
extern "C" JNIEXPORT jboolean JNICALL
Java_org_metroidprime_port_TouchControlsView_nativeTakePhysicalInput(JNIEnv*, jclass) {
  return sPhysicalInput.exchange(false, std::memory_order_acq_rel) ? JNI_TRUE : JNI_FALSE;
}
#endif
