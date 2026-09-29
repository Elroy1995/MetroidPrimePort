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
// Opt-in pickup-dump tour (MP_RANDO_SWEEP=1, with MP_RANDO_DUMP=1). Advance
// once per gameplay simulation tick; returns true when a restart was queued.
// Duplicate sweep requests and requests conflicting with a teleport are ignored.
void RequestWorldSweep();
bool ConsumeWorldSweepRequest(CStateManager& mgr);

// Disc image chosen on a previous launch, from the settings file. SetDiscPath
// marks the settings dirty so the choice is written back out on exit.
void LoadDiscPath();
const char* DiscPath();
void SetDiscPath(const char* path);

// Fast iteration
bool FastBoot();
bool SkipCutscenes();
void SetSkipCutscenes(bool enabled);
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
// MP_TURBO[=<ticks>]: lockstep for tests. Every loop runs exactly <ticks> fixed
// ticks (default 1, at most 16) and nothing waits for the wall clock, so a run
// goes as fast as the machine can render it; more ticks per frame skip
// presents, which is what limits a run under Xvfb. Game time stays exact per
// tick; audio and streams do not keep up. Not saved to the settings file.
bool Turbo();
unsigned TurboTicks();
// The dt of the simulation tick currently being processed, set by
// CGameArchitectureSupport::UpdateTicks. Game constants authored per 60 Hz tick
// (friction, damping) scale by this so they stay real-time at any rate.
float TickPeriod();
void SetTickPeriod(float dt);
// The same step expressed in 60 Hz frames (1.0 at 60 Hz). Per-tick counters and
// cadences add this instead of 1 so their real-time timing is unchanged.
float TickFrames();

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
// Current size of the game window in window coordinates; false before it exists.
bool WindowSize(int& width, int& height);
// Framebuffer width that is shown at 4:3 for the given height: the width itself
// in 4:3 mode (640x448 is displayed at 4:3), height * 4 / 3 otherwise, since the
// widened framebuffers have square pixels. Fits 4:3 art into a wider viewport.
inline int FourThreeWidth(int width, int height) {
  return AspectMode() == kAspect_4_3 ? width : height * 4 / 3;
}
// Widescreen HUD: keep each HUD element's shape but spread its position about
// the screen centre so edge elements reach the true wide corners. Only affects
// the aspect-matched in-game HUD frames.
bool HudWide();
void SetHudWide(bool enabled);
// First-person vertical field of view in degrees (retail 55). The arm cannon is
// drawn at the retail FOV whatever this is, like a view-model FOV.
const float kFovRetail = 55.f;
const float kFovMin = 45.f;
const float kFovMax = 90.f;
float FirstPersonFov();
void SetFirstPersonFov(float degrees);
// Multisample anti-aliasing (1 = off, or 4) and the max texture anisotropy
// (1-16, retail-style mipmapped textures ask for the max). Applied next frame.
int Msaa();
void SetMsaa(int samples);
int Anisotropy();
void SetAnisotropy(int level);
// Extras normally earned by finishing the game (or, for the Fusion Suit, by a
// GBA link to Metroid Fusion). They only change what the title screen offers;
// nothing is written into the save's persistent flags.
bool UnlockHardMode();
void SetUnlockHardMode(bool enabled);
bool UnlockFusionSuit();
void SetUnlockFusionSuit(bool enabled);
bool UnlockGalleries();
void SetUnlockGalleries(bool enabled);

// Mouse FPS mode owns aim only in playable first person; target locks retain
// their native camera and synchronize the mouse angles for a clean handoff.
// Motion/buttons are accepted only while SDL owns relative capture.
bool MouseAim();
void SetMouseAim(bool enabled);
// Twin-stick: the right stick aims the first-person camera directly (through the
// same aim state as the mouse) and is consumed, so it no longer drives the
// game's free-look. Works with or without mouse aim.
bool TwinStick();
void SetTwinStick(bool enabled);
// Right stick Y (-1..1) before twin-stick consumed it, for the Spring Ball;
// 0 when twin-stick is off (the game input still carries it then).
float TwinStickRightY();
void SetTwinStickRightY(float y);
// Spring Ball (C-stick up in morph ball, as in Metroid Prime Trilogy) once the
// Morph Ball Bombs are held. A connected Archipelago seed overrides it.
bool SpringBall();
void SetSpringBall(bool enabled);
// Fast Morph, as in Metroid Prime 4: short morph/unmorph transitions that keep
// momentum (capped at walking speed when unmorphing on the ground).
bool FastMorph();
void SetFastMorph(bool enabled);
// Toggle Lock-On: L latches until pressed again (lock-on, scan, strafe,
// grapple), and a lock that ends lets go by itself. Sticky Charge: letting go
// of a long A hold keeps the beam charging until the next press fires it. Both
// apply only unmorphed, in gameplay (port_hold_toggle.h).
bool LockOnToggle();
void SetLockOnToggle(bool enabled);
bool StickyCharge();
void SetStickyCharge(bool enabled);
// Spring Ball on a gyro flick (pad or phone tilted up sharply, like Trilogy's
// nunchuk flick), on top of C-stick up. Rate is the pitch speed in rad/s a flick
// must pass. The gyro source is the aim's.
bool SpringBallFlick();
void SetSpringBallFlick(bool enabled);
float SpringBallFlickRate();
void SetSpringBallFlickRate(float radiansPerSecond);
// True for a short while after a flick, so one just before landing still counts.
bool SpringBallFlickPending();
void ClearSpringBallFlick();
// Debug console: stand-in gyro rates (rad/s) instead of the sensors.
void SetGyroOverride(bool active, float pitch, float yaw);
// Aim travel in pixels per second at full stick deflection (scaled by the mouse
// sensitivity, so both share the same feel).
float StickAimRate();
void SetStickAimRate(float pixelsPerSecond);
// Gyro aiming. Mode: 0 off, 1 aim while the hold input is down, 2 always aim.
// Source: 0 auto (a pad's gyro if it has one, else the phone's), 1 controller
// only, 2 the device's own gyro (Android phones). Rate is aim pixels per second
// per radian per second of rotation, so it reads like the stick aim speed.
int GyroMode();
void SetGyroMode(int mode);
int GyroSource();
void SetGyroSource(int source);
float GyroRate();
void SetGyroRate(float pixelsPerSecondPerRad);
// Reads the gyro, feeds the aim and spots Spring Ball flicks. Call once per
// tick, before the frame.
void PollGyro();
// Short description of what the gyro is doing, for the overlay.
const char* GyroStatus();
// Feeds a normalised right-stick vector (-1..1, x right, y up) for this tick.
void AddStickAim(float x, float y, float dt);void SetMouseCaptured(bool captured);
bool MouseCaptured();
bool MouseGameplayActive();
void SetMouseGameplayActive(bool active);
bool MouseInvertX();
bool MouseInvertY();
bool MouseButtons();
bool MouseCrosshair();
unsigned MouseWeaponButtons(unsigned held);
// Outside mouse gameplay (morph ball, text boxes, menus) the left button is a
// plain A press; returns SDL_BUTTON_LMASK while it should be held.
unsigned MouseMenuButtons(unsigned held, bool focused);
// Real-mouse button state (SDL_BUTTON_*MASK), fed from the event loop. Touch-
// and pen-synthesised mouse buttons are left out; see PortMouse::HeldButtons.
void NoteMouseButton(bool synthetic, unsigned mask, bool down);
void ClearMouseButtons();
unsigned MouseHeldButtons();
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
// Writes the settings file now instead of waiting for the next overlay frame,
// so a disc chosen during startup is remembered even if no frame is drawn yet.
void SaveSettingsNow();
// Thread-safe snapshot of the overlay's visibility, for the Android touch
// controls. Unlike Visible() it performs no lazy initialization, so it is safe
// to call from the UI thread.
bool OverlayVisible();
// Thread-safe snapshot of the twin-stick setting, for the Android touch overlay
// to choose a controller layout. Like OverlayVisible(), performs no lazy
// initialization, so it is safe to call from the UI thread.
bool TwinStickFlag();
void Toggle();
// Asks for the overlay to be toggled on the next frame. Safe to call from any
// thread, unlike Toggle(), which touches ImGui state.
void RequestToggle();
// Feeds the pad into ImGui's gamepad navigation, applies any requested toggle,
// and handles F1. Call once per frame before the frame is built.
void UpdateControllerNav();

// Builds the debug windows for the current ImGui frame. Call once per presented
// frame, after Aurora begins the frame and before it ends it.
void DrawUI();

} // namespace PortDebug

#endif // METROID_PRIME_PORT_PORT_DEBUG_H
