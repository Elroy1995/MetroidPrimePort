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
// Rendering aspect ratio. kAspect_4_3 is the game's original 640x480.
// kAspect_16_9 widens to 16:9; kAspect_Window follows the window and updates
// live as it is resized.
enum EAspectMode {
  kAspect_4_3 = 0,
  kAspect_16_9,
  kAspect_Window,
};
EAspectMode AspectMode();
void SetAspectMode(EAspectMode mode);

// Mouse aim: relative mouse drives the first-person free-look directly.
bool MouseAim();
void SetMouseAim(bool enabled);
float MouseSensitivity();
void SetMouseSensitivity(float radiansPerPixel);
void AddMouseDelta(float dx, float dy);
// Returns true and writes the accumulated delta since the last call.
bool ConsumeMouseDelta(float& dx, float& dy);

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
