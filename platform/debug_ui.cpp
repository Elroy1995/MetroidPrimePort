// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"
#include "port_mouse.h"

#include <aurora/gfx.h>
#include <dolphin/vi.h>
#include <imgui.h>

#include <cstdlib>
#include <cstring>
#include <cmath>

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

void EnsureInitialized() {
  if (sInitialized) {
    return;
  }
  sInitialized = true;
  sFastBoot = std::getenv("MP_FAST_BOOT") != nullptr;
  sSkipCutscenes = std::getenv("MP_SKIP_CUTSCENES") != nullptr;
  sVisible = std::getenv("MP_SHOW_DEBUG_UI") != nullptr;
  if (const char* aspect = std::getenv("MP_ASPECT")) {
    if (std::strcmp(aspect, "16:9") == 0) {
      sAspectMode = PortDebug::kAspect_16_9;
    } else if (std::strcmp(aspect, "window") == 0) {
      sAspectMode = PortDebug::kAspect_Window;
    }
  } else if (std::getenv("MP_WIDESCREEN") != nullptr) {
    sAspectMode = PortDebug::kAspect_16_9;
  }
  sMouseAim = std::getenv("MP_MOUSE_AIM") != nullptr;
  sMouseInvertX = std::getenv("MP_MOUSE_INVERT_X") != nullptr;
  sMouseInvertY = std::getenv("MP_MOUSE_INVERT_Y") != nullptr;
  sMouseButtons = std::getenv("MP_DISABLE_MOUSE_BUTTONS") == nullptr;
  sMouseCrosshair = std::getenv("MP_DISABLE_MOUSE_CROSSHAIR") == nullptr;
  if (const char* sens = std::getenv("MP_MOUSE_SENS")) {
    const float value = static_cast< float >(std::atof(sens));
    if (std::isfinite(value) && value > 0.f) {
      sMouseSensitivity = value;
    }
  }
  sAiAudioEnabled = std::getenv("MP_DISABLE_AI_AUDIO") == nullptr;
  if (const char* speed = std::getenv("MP_CUTSCENE_SPEED")) {
    const float value = static_cast< float >(std::atof(speed));
    if (std::isfinite(value) && value >= 1.f && value <= 32.f) {
      sCutsceneSpeed = value;
    }
  }
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

void SetFrameLimitEnabled(bool enabled) {
  EnsureInitialized();
  sFrameLimitEnabled = enabled;
}

bool VsyncEnabled() {
  EnsureInitialized();
  return sVsyncEnabled;
}

void SetVsyncEnabled(bool enabled) {
  EnsureInitialized();
  if (sVsyncEnabled == enabled) {
    return;
  }
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
  return sMouseAimState.Update(active, locked, x, y, z, sMouseFrameX, sMouseFrameY,
                               MouseSensitivity(), MouseInvertX(), MouseInvertY());
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
  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("Frame limit (60 FPS)", &frameLimit)) {
    sFrameLimitEnabled = frameLimit;
  }
  ImGui::Text("FPS: %.1f", static_cast< double >(ImGui::GetIO().Framerate));
  ImGui::Text("Frame time: %.2f ms", static_cast< double >(ImGui::GetIO().DeltaTime) * 1000.0);
}

void DrawCutscenesTab() {
  ImGui::Checkbox("Skip / fast-forward cutscenes", &sSkipCutscenes);
  ImGui::SliderFloat("Cutscene speed", &sCutsceneSpeed, 1.f, 32.f, "%.0fx");
  if (ImGui::Button("Reset cutscene speed")) {
    sCutsceneSpeed = 8.f;
  }
}

void DrawRenderTab() {
  bool vsync = sVsyncEnabled;
  if (ImGui::Checkbox("Vsync", &vsync)) {
    SetVsyncEnabled(vsync);
  }

  int aspect = static_cast< int >(sAspectMode);
  if (ImGui::Combo("Aspect ratio", &aspect, "4:3\0" "16:9\0" "Follow window\0")) {
    SetAspectMode(static_cast< EAspectMode >(aspect));
  }

  bool autoScale = sRenderScale <= 0.f;
  if (ImGui::Checkbox("Auto render scale (native)", &autoScale)) {
    SetRenderScale(autoScale ? 0.f : 1.f);
  }
  if (!autoScale) {
    float scale = sRenderScale;
    if (ImGui::SliderFloat("EFB scale", &scale, 1.f, 2.f, "%.2fx")) {
      SetRenderScale(scale);
    }
    ImGui::TextUnformatted("Scales the internal EFB; higher values use more GPU memory.");
  }
}

void DrawInputTab() {
  bool mouseAim = sMouseAim;
  if (ImGui::Checkbox("Mouse aim", &mouseAim)) {
    SetMouseAim(mouseAim);
  }
  ImGui::Checkbox("Invert mouse X", &sMouseInvertX);
  ImGui::Checkbox("Invert mouse Y", &sMouseInvertY);
  if (ImGui::Checkbox("Mouse weapon buttons", &sMouseButtons)) {
    sMouseButtonGate.Reset();
  }
  ImGui::Checkbox("Mouse-aim crosshair", &sMouseCrosshair);
  ImGui::TextUnformatted("Left: fire/charge   Right: lock-on   Middle: missile");
  ImGui::TextUnformatted("Existing keyboard/controller weapon bindings also work.");
  ImGui::SliderFloat("Sensitivity", &sMouseSensitivity, 0.0005f, 0.02f, "%.4f rad/px",
                     ImGuiSliderFlags_Logarithmic);
}

void DrawAudioTab() {
  bool ai = AiAudioEnabled();
  if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
    SetAiAudioEnabled(ai);
  }
  bool musyx = sMusyxAudioEnabled;
  if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
    SetMusyxAudioEnabled(musyx);
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
}

void DrawUI() {
  EnsureInitialized();
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
}

} // namespace PortDebug
