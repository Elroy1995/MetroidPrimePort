// In-game debug overlay. The port draws it with Aurora's ImGui backend, which
// is already initialized and rendered every presented frame, so this only has
// to build the windows between aurora_begin_frame and aurora_end_frame.

#include "port_debug.h"

#include <aurora/gfx.h>
#include <dolphin/vi.h>
#include <imgui.h>

#include <cstdlib>

namespace aurora {
void request_screenshot() noexcept;
}

// Implemented by the AI and MusyX audio backends.
extern "C" void AIPortSetOutputEnabled(int enabled);
extern "C" void salSetMuted(int muted);

namespace {
bool sInitialized = false;
bool sFastBoot = false;
bool sSkipCutscenes = false;
float sCutsceneSpeed = 8.f;
bool sFrameLimitEnabled = true;
bool sVsyncEnabled = false;
float sRenderScale = 1.f;
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
  sAiAudioEnabled = std::getenv("MP_DISABLE_AI_AUDIO") == nullptr;
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

bool AiAudioEnabled() {
  EnsureInitialized();
  return sAiAudioEnabled;
}

void SetAiAudioEnabled(bool enabled) {
  EnsureInitialized();
  if (sAiAudioEnabled == enabled) {
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
}

void DrawUI() {
  EnsureInitialized();
  if (!sVisible) {
    return;
  }

  ImGui::SetNextWindowPos(ImVec2(12.f, 12.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(340.f, 0.f), ImGuiCond_FirstUseEver);
  ImGui::Begin("Metroid Prime Port");

  ImGui::TextUnformatted("F1 to hide");
  ImGui::Separator();

  if (ImGui::CollapsingHeader("Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
    bool frameLimit = sFrameLimitEnabled;
    if (ImGui::Checkbox("Frame limit (60 FPS)", &frameLimit)) {
      sFrameLimitEnabled = frameLimit;
    }
    ImGui::Text("FPS: %.1f", static_cast< double >(ImGui::GetIO().Framerate));
  }

  if (ImGui::CollapsingHeader("Cutscenes")) {
    ImGui::Checkbox("Skip / fast-forward cutscenes", &sSkipCutscenes);
    ImGui::SliderFloat("Cutscene speed", &sCutsceneSpeed, 1.f, 32.f, "%.0fx");
    if (ImGui::Button("Reset cutscene speed")) {
      sCutsceneSpeed = 8.f;
    }
  }

  if (ImGui::CollapsingHeader("Render")) {
    bool vsync = sVsyncEnabled;
    if (ImGui::Checkbox("Vsync", &vsync)) {
      SetVsyncEnabled(vsync);
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
      ImGui::TextUnformatted("Scales the internal EFB; higher values use more MEM1.");
    }
  }

  if (ImGui::CollapsingHeader("Audio")) {
    bool ai = sAiAudioEnabled;
    if (ImGui::Checkbox("Streamed audio (music/movies)", &ai)) {
      SetAiAudioEnabled(ai);
    }
    bool musyx = sMusyxAudioEnabled;
    if (ImGui::Checkbox("MusyX audio (effects/streams)", &musyx)) {
      SetMusyxAudioEnabled(musyx);
    }
  }

  if (ImGui::CollapsingHeader("Session")) {
    if (ImGui::Button("Restart to menu")) {
      RequestReset();
    }
    ImGui::SameLine();
    if (ImGui::Button("Screenshot (F12)")) {
      aurora::request_screenshot();
    }
  }

  ImGui::End();
}

} // namespace PortDebug
