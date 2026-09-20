#ifndef METROID_PRIME_PORT_PORT_DEBUG_H
#define METROID_PRIME_PORT_PORT_DEBUG_H
#include <cstdint>

// Runtime debug settings shared between the platform layer and the game.
// Defaults come from environment variables so existing workflows keep working,
// and the in-game debug window can toggle them live.

class CStateManager;

namespace PortDebug {

// Live game state for the debug menu (set every simulation tick by
// CStateManager::Update). Null before gameplay starts.
void SetStateManager(CStateManager* mgr);
CStateManager* StateManager();
// Requests an area change; consumed and executed by the game update so it does
// not run from the render/UI path.
void RequestTeleport(int areaId);
bool ConsumeTeleportRequest(int& areaId);
// Requests a jump to a different world (MLVL). areaAssetId is a MREA asset id,
// or 0 to land in the world's default area. The game update runs the same
// restart the in-game world teleporters use, so the world is fully reloaded.
void RequestWorldTeleport(uint32_t worldId, uint32_t areaAssetId);
bool ConsumeWorldTeleportRequest(uint32_t& worldId, uint32_t& areaAssetId);

// Fast iteration
bool FastBoot();
bool SkipCutscenes();
float CutsceneSpeed();
// Simulation tick rate. 60 is console-accurate; higher values run the tick at
// the display rate instead of interpolating presentation. Experimental.
unsigned SimRate();
void SetSimRate(unsigned hz);
// The current simulation step in seconds (1 / SimRate()).
float SimPeriod();
// When enabled the simulation step follows the measured frame time (clamped to
// a sane range) instead of SimRate(), so a variable frame rate is matched
// tick-for-tick. Experimental.
bool SimAdaptive();
void SetSimAdaptive(bool enabled);

// Presentation
bool FrameLimitEnabled();
void SetFrameLimitEnabled(bool enabled);
void RecordFrame(uint64_t durationNs, unsigned ticks, bool presented);
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

// Mouse FPS mode owns aim only in playable first person; target locks retain
// their native camera and synchronize the mouse angles for a clean handoff.
// Motion/buttons are accepted only while SDL owns relative capture.
bool MouseAim();
void SetMouseAim(bool enabled);
void SetMouseCaptured(bool captured);
bool MouseCaptured();
bool MouseGameplayActive();
void SetMouseGameplayActive(bool active);
bool MouseInvertX();
bool MouseInvertY();
bool MouseButtons();
bool MouseCrosshair();
unsigned MouseWeaponButtons(unsigned held);
// Called during simulation, using the effective unbobbed camera direction.
bool UpdateMouseAim(bool active, bool locked, float x, float y, float z);
void SynchronizeMouseAim(float x, float y, float z);
void ResetMouseAim();
float MouseSensitivity();
void SetMouseSensitivity(float radiansPerPixel);
// Called from the input event loop as relative motion arrives.
void AddMouseDelta(float dx, float dy);
// Called once per simulated frame to latch the deltas for that frame.
void BeginFrameMouse();
void GetFrameMouseDelta(float& dx, float& dy);
// Mouse-look camera angles (radians): world yaw and pitch. The game owns the
// update and applies them to the first-person camera.
float AimYaw();
float AimPitch();
bool AimInitialized();

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
