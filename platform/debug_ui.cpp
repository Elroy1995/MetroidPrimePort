// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"

#include <imgui.h>

#include <cstdlib>

namespace aurora {
void request_screenshot() noexcept;
}

namespace {
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
bool sFrameLimitEnabled = true;
bool sVisible = false;

void EnsureInitialized() {
  if (sInitialized) {
    return;
  }
  sInitialized = true;
  sFastBoot = std::getenv("MP_FAST_BOOT") != nullptr;
  sSkipCutscenes = std::getenv("MP_SKIP_CUTSCENES") != nullptr;
  sVisible = std::getenv("MP_SHOW_DEBUG_UI") != nullptr;
  if (const char* speed = std::getenv("MP_CUTSCENE_SPEED")) {
    const float value = static_cast< float >(std::atof(speed));
    if (value >= 1.f) {
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

bool Visible() {
  EnsureInitialized();
  return sVisible;
}

void Toggle() {
  EnsureInitialized();
  sVisible = !sVisible;
}

void DrawUI() {
  EnsureInitialized();
  if (!sVisible) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(320.f, 0.f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Metroid Prime Port");

  ImGui::TextUnformatted("F1 to hide");
  ImGui::Separator();

  bool frameLimit = sFrameLimitEnabled;
  if (ImGui::Checkbox("Frame limit (60 FPS)", &frameLimit)) {
    sFrameLimitEnabled = frameLimit;
  }

  ImGui::Checkbox("Skip / fast-forward cutscenes", &sSkipCutscenes);
  ImGui::SliderFloat("Cutscene speed", &sCutsceneSpeed, 1.f, 32.f, "%.0fx");
  if (ImGui::Button("Reset cutscene speed")) {
    sCutsceneSpeed = 8.f;
  }

  ImGui::Separator();
  ImGui::Text("FPS: %.1f", static_cast< double >(ImGui::GetIO().Framerate));
  if (ImGui::Button("Screenshot (F12)")) {
    aurora::request_screenshot();
  }

  ImGui::End();
}

} // namespace PortDebug
