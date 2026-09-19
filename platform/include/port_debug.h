#ifndef METROID_PRIME_PORT_PORT_DEBUG_H
#define METROID_PRIME_PORT_PORT_DEBUG_H

// Runtime debug settings shared between the platform layer and the game.
// Defaults come from environment variables so existing workflows keep working,
// and the in-game debug window can toggle them live.

namespace PortDebug {

// Fast iteration
bool FastBoot();
bool SkipCutscenes();
float CutsceneSpeed();

// Presentation
bool FrameLimitEnabled();
void SetFrameLimitEnabled(bool enabled);
bool VsyncEnabled();
void SetVsyncEnabled(bool enabled);
// 0 = auto (native, driven by the display scale), otherwise a fixed multiplier.
float RenderScale();
void SetRenderScale(float scale);
// Renders a 16:9 image (wider horizontal FOV). Startup-only: the game's render
// mode is configured before the first frame, so changing it needs a restart.
bool Widescreen();
void SetWidescreen(bool enabled);

// Audio paths
bool AiAudioEnabled();
void SetAiAudioEnabled(bool enabled);
bool MusyxAudioEnabled();
void SetMusyxAudioEnabled(bool enabled);

// Session
void RequestReset();
bool ConsumeResetRequest();

bool Visible();
void Toggle();

// Builds the debug windows for the current ImGui frame. Call once per presented
// frame, after Aurora begins the frame and before it ends it.
void DrawUI();

} // namespace PortDebug

#endif // METROID_PRIME_PORT_PORT_DEBUG_H
