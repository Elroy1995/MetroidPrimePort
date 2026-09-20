// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"
#include "port_mouse.h"
#include "port_build_info.h"

#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CMemoryCard.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"

#include <aurora/gfx.h>
#include <dolphin/vi.h>
#include <imgui.h>

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_stdinc.h>

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
bool sFrameLimitEnabled = true;
bool sTraceTiming = false;
uint64_t sTimingNs = 0;
unsigned sTimingFrames = 0, sTimingTicks = 0;
double sActualFps = 0.0, sActualTps = 0.0;
bool sVsyncEnabled = false;
float sRenderScale = 1.f;
PortDebug::EAspectMode sAspectMode = PortDebug::kAspect_4_3;
bool sMouseAim = false;
bool sMouseCaptured = false;
bool sMouseGameplayActive = false;
bool sMouseInvertX = false;
bool sMouseInvertY = false;
bool sMouseButtons = true;
bool sMouseCrosshair = true;
PortMouse::AimState sMouseAimState;
PortMouse::ButtonGate sMouseButtonGate;
float sMouseSensitivity = 0.0035f;
float sMousePendingX = 0.f;
float sMousePendingY = 0.f;
float sMouseFrameX = 0.f;
float sMouseFrameY = 0.f;
bool sAiAudioEnabled = true;
bool sMusyxAudioEnabled = true;
bool sResetRequested = false;
bool sVisible = false;
bool sSettingsDirty = false;
bool sAudioSettingsApplied = false;
bool sPresentationSettingsApplied = false;
CStateManager* sStateManager = nullptr;
int sPendingTeleport = -1;
bool sHasWorldTeleport = false;
uint32_t sWorldTeleportWorld = 0;
uint32_t sWorldTeleportArea = 0;

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
  } else if (key == "mouse_aim") {
    sMouseAim = ParseBool(value);
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
  } else if (key == "ai_audio") {
    sAiAudioEnabled = ParseBool(value);
  } else if (key == "musyx_audio") {
    sMusyxAudioEnabled = ParseBool(value);
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
  file << "vsync=" << (sVsyncEnabled ? 1 : 0) << '\n';
  file << "render_scale=" << sRenderScale << '\n';
  file << "frame_limit=" << (sFrameLimitEnabled ? 1 : 0) << '\n';
  file << "skip_cutscenes=" << (sSkipCutscenes ? 1 : 0) << '\n';
  file << "cutscene_speed=" << sCutsceneSpeed << '\n';
  file << "mouse_aim=" << (sMouseAim ? 1 : 0) << '\n';
  file << "mouse_invert_x=" << (sMouseInvertX ? 1 : 0) << '\n';
  file << "mouse_invert_y=" << (sMouseInvertY ? 1 : 0) << '\n';
  file << "mouse_buttons=" << (sMouseButtons ? 1 : 0) << '\n';
  file << "mouse_crosshair=" << (sMouseCrosshair ? 1 : 0) << '\n';
  file << "mouse_sensitivity=" << sMouseSensitivity << '\n';
  file << "ai_audio=" << (sAiAudioEnabled ? 1 : 0) << '\n';
  file << "musyx_audio=" << (sMusyxAudioEnabled ? 1 : 0) << '\n';
  file.flush();
  std::fprintf(stderr, "metroid_prime_port: saved settings to %s\n", path.c_str());
  sSettingsDirty = false;
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
  if (std::getenv("MP_MOUSE_AIM") != nullptr) {
    sMouseAim = true;
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

float CutsceneSpeed() {
  EnsureInitialized();
  return sCutsceneSpeed;
}

bool FrameLimitEnabled() {
  EnsureInitialized();
  return sFrameLimitEnabled;
}

void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented) {
  sTimingNs += durationNs;
  sTimingTicks += ticks;
  if (presented) ++sTimingFrames;
  if (sTimingNs >= 1000000000ull) {
    const double seconds = static_cast<double>(sTimingNs) / 1000000000.0;
    sActualFps = sTimingFrames / seconds;
    sActualTps = sTimingTicks / seconds;
    if (sTraceTiming) {
      std::fprintf(stderr, "[timing] render=%.1f FPS simulation=%.1f ticks/s cap=%s\n",
                   sActualFps, sActualTps, sFrameLimitEnabled ? "60" : "off");
    }
    sTimingNs = 0;
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

bool Visible() {
  EnsureInitialized();
  return sVisible;
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
  ImGui::Text("Measured render rate: %.1f FPS", sActualFps);
  ImGui::Text("Measured simulation: %.1f ticks/s (target 60)", sActualTps);
  ImGui::Text("Frame time: %.2f ms", static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);
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
}

void DrawInputTab() {
  bool mouseAim = sMouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
    MarkDirty();
  }
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
    aurora_enable_vsync(sVsyncEnabled);
  }
  if (!sVisible) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(8.f, 8.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(440.f, 200.f), ImGuiCond_FirstUseEver);
  bool open = true;
  if (ImGui::Begin("Metroid Prime Port", &open, ImGuiWindowFlags_MenuBar)) {
    if (ImGui::BeginMenuBar()) {
      ImGui::TextUnformatted("F1: hide   F10: frame limit   F12: screenshot");
      ImGui::EndMenuBar();
    }

    if (ImGui::BeginTabBar("##debug_tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      if (ImGui::BeginTabItem("Performance")) {
        DrawPerformanceTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Cutscenes")) {
        DrawCutscenesTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Input")) {
        DrawInputTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Render")) {
        DrawRenderTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Audio")) {
        DrawAudioTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Debug")) {
        DrawDebugTab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Session")) {
        DrawSessionTab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  if (!open) {
    sVisible = false;
  }

  if (sSettingsDirty) {
    SaveSettings();
  }
}

} // namespace PortDebug
