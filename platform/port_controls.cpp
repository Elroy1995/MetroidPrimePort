// Controls tab: rebinds keyboard/mouse and controller inputs to the emulated
// pad. The binding backend (matching, persistence, name helpers) is Aurora's.

#include "port_controls.h"
#include "port_debug.h"

#include "MetroidPrime/CControlMapper.hpp"
#include "MetroidPrime/Tweaks/CTweakPlayerControl.hpp"
#include "MetroidPrime/Tweaks/CTweaks.hpp"

#include <dolphin/pad.h>
#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <iterator>
#include <string>

namespace {

constexpr u32 kControlPort = PAD_CHAN0;
// A capture that sees no input for this long gives up, so a stray Bind click
// never leaves the tab listening (or a held input leaves it disabled).
constexpr Uint64 kCaptureTimeoutMs = 5000;
// Half travel, as PADGetNativeAxisPulled uses.
constexpr Sint16 kAxisPullThreshold = 16384;

typedef ControlMapper::EFunctionList EFunctionList;

struct SControlPadButton {
  PADButton button;
  const char* label;
  EFunctionList function;
};
const SControlPadButton kControlPadButtons[] = {
    {PAD_BUTTON_A, "A", ControlMapper::kFL_AButton},
    {PAD_BUTTON_B, "B", ControlMapper::kFL_BButton},
    {PAD_BUTTON_X, "X", ControlMapper::kFL_XButton},
    {PAD_BUTTON_Y, "Y", ControlMapper::kFL_YButton},
    {PAD_TRIGGER_L, "L", ControlMapper::kFL_LeftTriggerPress},
    {PAD_TRIGGER_R, "R", ControlMapper::kFL_RightTriggerPress},
    {PAD_TRIGGER_Z, "Z", ControlMapper::kFL_ZButton},
    {PAD_BUTTON_START, "Start", ControlMapper::kFL_Start},
    {PAD_BUTTON_UP, "D-pad Up", ControlMapper::kFL_DPadUp},
    {PAD_BUTTON_DOWN, "D-pad Down", ControlMapper::kFL_DPadDown},
    {PAD_BUTTON_LEFT, "D-pad Left", ControlMapper::kFL_DPadLeft},
    {PAD_BUTTON_RIGHT, "D-pad Right", ControlMapper::kFL_DPadRight},
};

// Indexed by PADAxis.
struct SControlPadAxis {
  const char* label;
  EFunctionList function;
};
const SControlPadAxis kControlPadAxes[PAD_AXIS_COUNT] = {
    {"Stick Right", ControlMapper::kFL_LeftStickRight},
    {"Stick Left", ControlMapper::kFL_LeftStickLeft},
    {"Stick Up", ControlMapper::kFL_LeftStickUp},
    {"Stick Down", ControlMapper::kFL_LeftStickDown},
    {"C-Stick Right", ControlMapper::kFL_RightStickRight},
    {"C-Stick Left", ControlMapper::kFL_RightStickLeft},
    {"C-Stick Up", ControlMapper::kFL_RightStickUp},
    {"C-Stick Down", ControlMapper::kFL_RightStickDown},
    {"L Analog", ControlMapper::kFL_LeftTrigger},
    {"R Analog", ControlMapper::kFL_RightTrigger},
};

// Game commands under the names players know, headline actions first: a row
// is labelled with the first one the game maps its pad input to (the mapping
// is the disc's CTweakPlayerControl, so the labels follow it).
struct SCommandName {
  ControlMapper::ECommands command;
  const char* name;
};
const SCommandName kCommandNames[] = {
    {ControlMapper::kC_FireOrBomb, "Fire / Bomb"},
    {ControlMapper::kC_JumpOrBoost, "Jump / Boost"},
    {ControlMapper::kC_MissileOrPowerBomb, "Missile / Power Bomb"},
    {ControlMapper::kC_Morph, "Morph Ball"},
    {ControlMapper::kC_OrbitObject, "Lock On"},
    {ControlMapper::kC_LookHold1, "Free Look"},
    {ControlMapper::kC_ScanItem, "Scan"},
    {ControlMapper::kC_SpiderBall, "Spider Ball"},
    {ControlMapper::kC_NoVisor, "Combat Visor"},
    {ControlMapper::kC_EnviroVisor, "Scan Visor"},
    {ControlMapper::kC_ThermoVisor, "Thermal Visor"},
    {ControlMapper::kC_XrayVisor, "X-Ray Visor"},
    {ControlMapper::kC_PowerBeam, "Power Beam"},
    {ControlMapper::kC_IceBeam, "Ice Beam"},
    {ControlMapper::kC_WaveBeam, "Wave Beam"},
    {ControlMapper::kC_PlasmaBeam, "Plasma Beam"},
    {ControlMapper::kC_Forward, "Forward"},
    {ControlMapper::kC_Backward, "Back"},
    {ControlMapper::kC_TurnLeft, "Turn Left"},
    {ControlMapper::kC_TurnRight, "Turn Right"},
    {ControlMapper::kC_StrafeLeft, "Strafe Left"},
    {ControlMapper::kC_StrafeRight, "Strafe Right"},
    {ControlMapper::kC_LookUp, "Look Up"},
    {ControlMapper::kC_LookDown, "Look Down"},
    {ControlMapper::kC_LookLeft, "Look Left"},
    {ControlMapper::kC_LookRight, "Look Right"},
};

// "Fire / Bomb (A)", or just the pad label for an input no command uses (or
// before the tweaks load).
std::string ActionLabel(EFunctionList function, const char* padLabel) {
  if (gpTweakPlayerControlCurrent != nullptr) {
    for (const SCommandName& entry : kCommandNames) {
      if (gpTweakPlayerControlCurrent->GetMapping(entry.command) == function) {
        return std::string(entry.name) + " (" + padLabel + ")";
      }
    }
  }
  // The game reads these directly rather than through the command mapping
  // (CMFGame: Z opens the map, Start pauses).
  switch (function) {
  case ControlMapper::kFL_ZButton:
    return std::string("Map (") + padLabel + ")";
  case ControlMapper::kFL_Start:
    return std::string("Pause (") + padLabel + ")";
  default:
    return padLabel;
  }
}

struct SMouseCode {
  Uint32 button;
  s32 code;
};
const SMouseCode kMouseCodes[] = {
    {SDL_BUTTON_LEFT, PAD_KEY_MOUSE_LEFT}, {SDL_BUTTON_MIDDLE, PAD_KEY_MOUSE_MIDDLE},
    {SDL_BUTTON_RIGHT, PAD_KEY_MOUSE_RIGHT}, {SDL_BUTTON_X1, PAD_KEY_MOUSE_X1},
    {SDL_BUTTON_X2, PAD_KEY_MOUSE_X2},
};

enum class ECapture { kNone, kKeyButton, kKeyAxis, kPadButton, kPadAxis };

// One physical input: a key or mouse button (the negative PAD_KEY_MOUSE_*
// codes), a controller button, or one direction of a controller axis.
struct SInput {
  enum EKind { kKey, kPadButton, kPadAxis };
  EKind kind = kKey;
  s32 code = PAD_KEY_INVALID;
  PADAxisSign sign = AXIS_SIGN_POSITIVE;
};

struct SCapture {
  ECapture target = ECapture::kNone;
  int index = 0;
  // Which of a keyboard row's PAD_KEY_SLOT_COUNT keys; always 0 for the pad.
  int slot = 0;
  Uint64 startMs = 0;
  // Inputs held when the capture started, the Bind click or pad press among
  // them. Each only counts once it has been released and pressed again.
  bool heldKeys[SDL_SCANCODE_COUNT] = {};
  Uint32 heldMouse = 0;
  bool heldPadButtons[SDL_GAMEPAD_BUTTON_COUNT] = {};
  bool heldPadAxes[SDL_GAMEPAD_AXIS_COUNT][2] = {};
  // After binding, the rows stay disabled until the bound input is released,
  // so the same press can't also activate the widget under the cursor or the
  // nav focus (Enter, or the pad's A) and start another capture.
  bool settling = false;
  SInput bound;
  // The captured input already drives another row: the tab asks whether to
  // swap, bind it to both or cancel. Its buttons wait for the input's release.
  bool conflict = false;
  bool conflictReleased = false;
  ECapture otherKind = ECapture::kNone;
  int otherIndex = 0;
  int otherSlot = 0;
  // Screen rect of the listening row's Press... button, from the last frame.
  ImVec2 pressMin{0.f, 0.f};
  ImVec2 pressMax{0.f, 0.f};
};
SCapture sCapture;
// ImGui frame the tab was last drawn on.
int sLastDrawFrame = -1;

SDL_Gamepad* PortGamepad() {
  const s32 index = PADGetIndexForPort(kControlPort);
  return index >= 0 ? PADGetSDLGamepadForIndex(static_cast< u32 >(index)) : nullptr;
}

// Only a real mouse: touches and pens also arrive as mouse buttons, and a tap on
// the overlay shouldn't bind "Mouse Left".
Uint32 MouseButtons() { return PortDebug::MouseHeldButtons(); }

bool AxisPulled(SDL_Gamepad* pad, int axis, PADAxisSign sign) {
  const Sint16 value = SDL_GetGamepadAxis(pad, static_cast< SDL_GamepadAxis >(axis));
  if (sign == AXIS_SIGN_POSITIVE) {
    return value >= kAxisPullThreshold;
  }
  // Triggers only pull one way.
  if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    return false;
  }
  return value <= -kAxisPullThreshold;
}

bool InputHeld(const SInput& input) {
  switch (input.kind) {
  case SInput::kKey:
    if (input.code >= 0) {
      int count = 0;
      const bool* keys = SDL_GetKeyboardState(&count);
      return input.code < count && keys[input.code];
    }
    for (const SMouseCode& mouse : kMouseCodes) {
      if (mouse.code == input.code) {
        return (MouseButtons() & SDL_BUTTON_MASK(mouse.button)) != 0;
      }
    }
    return false;
  case SInput::kPadButton: {
    SDL_Gamepad* pad = PortGamepad();
    return pad != nullptr && SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(input.code));
  }
  case SInput::kPadAxis: {
    SDL_Gamepad* pad = PortGamepad();
    return pad != nullptr && AxisPulled(pad, input.code, input.sign);
  }
  }
  return false;
}

void StartCapture(ECapture target, int index, int slot) {
  sCapture = SCapture{};
  sCapture.target = target;
  sCapture.index = index;
  sCapture.slot = slot;
  sCapture.startMs = SDL_GetTicks();
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) {
    sCapture.heldKeys[i] = keys[i];
  }
  sCapture.heldMouse = MouseButtons();
  if (SDL_Gamepad* pad = PortGamepad()) {
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
      sCapture.heldPadButtons[i] = SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(i));
    }
    for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
      sCapture.heldPadAxes[i][0] = AxisPulled(pad, i, AXIS_SIGN_NEGATIVE);
      sCapture.heldPadAxes[i][1] = AxisPulled(pad, i, AXIS_SIGN_POSITIVE);
    }
  }
}

void CancelCapture() { sCapture = SCapture{}; }

// Returns the first input of the capture's kind that is held now but wasn't
// when the capture started; clears the start-of-capture marks as inputs are
// released.
bool NewInput(SInput& out) {
  int count = 0;
  const bool* keys = SDL_GetKeyboardState(&count);
  bool found = false;
  const bool wantKeys =
      sCapture.target == ECapture::kKeyButton || sCapture.target == ECapture::kKeyAxis;
  for (int i = 0; i < count && i < SDL_SCANCODE_COUNT; ++i) {
    sCapture.heldKeys[i] = sCapture.heldKeys[i] && keys[i];
    // Esc is reported for every capture, since it cancels.
    if ((wantKeys || i == SDL_SCANCODE_ESCAPE) && !found && keys[i] && !sCapture.heldKeys[i]) {
      out = {SInput::kKey, i, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }
  const Uint32 mouse = MouseButtons();
  sCapture.heldMouse &= mouse;
  for (const SMouseCode& code : kMouseCodes) {
    const Uint32 mask = SDL_BUTTON_MASK(code.button);
    if (wantKeys && !found && (mouse & mask) != 0 && (sCapture.heldMouse & mask) == 0) {
      out = {SInput::kKey, code.code, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }

  SDL_Gamepad* pad = PortGamepad();
  if (pad == nullptr) {
    return found;
  }
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    const bool held = SDL_GetGamepadButton(pad, static_cast< SDL_GamepadButton >(i));
    sCapture.heldPadButtons[i] = sCapture.heldPadButtons[i] && held;
    if (sCapture.target == ECapture::kPadButton && !found && held && !sCapture.heldPadButtons[i]) {
      out = {SInput::kPadButton, i, AXIS_SIGN_POSITIVE};
      found = true;
    }
  }
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
    for (int s = 0; s < 2; ++s) {
      const PADAxisSign sign = s == 0 ? AXIS_SIGN_NEGATIVE : AXIS_SIGN_POSITIVE;
      const bool pulled = AxisPulled(pad, i, sign);
      sCapture.heldPadAxes[i][s] = sCapture.heldPadAxes[i][s] && pulled;
      if (sCapture.target == ECapture::kPadAxis && !found && pulled && !sCapture.heldPadAxes[i][s]) {
        out = {SInput::kPadAxis, i, sign};
        found = true;
      }
    }
  }
  return found;
}

// Points one row's slot at an input (code -1 unbinds a key or button row).
// Doesn't save.
void BindRow(ECapture kind, int index, int slot, const SInput& input) {
  switch (kind) {
  case ECapture::kKeyButton: {
    PADKeyButtonBinding binding{};
    binding.scancode = input.code;
    binding.padButton = kControlPadButtons[index].button;
    PADSetKeyButtonBindingSlot(kControlPort, static_cast< u32 >(slot), binding);
    // A binding on a switched-off keyboard would never fire.
    PADSetKeyboardActive(kControlPort, TRUE);
    break;
  }
  case ECapture::kKeyAxis: {
    PADKeyAxisBinding binding{};
    binding.scancode = input.code;
    binding.padAxis = static_cast< PADAxis >(index);
    binding.influence = 1;
    PADSetKeyAxisBindingSlot(kControlPort, static_cast< u32 >(slot), binding);
    PADSetKeyboardActive(kControlPort, TRUE);
    break;
  }
  case ECapture::kPadButton: {
    PADButtonMapping mapping{};
    mapping.nativeButton = static_cast< u32 >(input.code);
    mapping.padButton = kControlPadButtons[index].button;
    PADSetButtonMapping(kControlPort, mapping);
    break;
  }
  case ECapture::kPadAxis: {
    PADAxisMapping mapping{};
    mapping.nativeAxis = {input.code, input.sign};
    mapping.nativeButton = static_cast< s32 >(PAD_NATIVE_BUTTON_INVALID);
    mapping.padAxis = static_cast< PADAxis >(index);
    PADSetAxisMapping(kControlPort, mapping);
    break;
  }
  case ECapture::kNone:
    break;
  }
}

s32 KeyForPadButton(const PADKeyButtonBinding* list, u32 count, PADButton button);
s32 KeyForPadAxis(const PADKeyAxisBinding* list, u32 count, PADAxis axis);
u32 NativeButtonForPadButton(const PADButtonMapping* list, u32 count, PADButton button);

// What a row's slot is bound to now; code -1 when nothing.
SInput RowInput(ECapture kind, int index, int slot) {
  u32 count = 0;
  switch (kind) {
  case ECapture::kKeyButton: {
    const PADKeyButtonBinding* list = PADGetKeyButtonBindingsSlot(kControlPort, static_cast< u32 >(slot), &count);
    return {SInput::kKey, KeyForPadButton(list, count, kControlPadButtons[index].button), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kKeyAxis: {
    const PADKeyAxisBinding* list = PADGetKeyAxisBindingsSlot(kControlPort, static_cast< u32 >(slot), &count);
    return {SInput::kKey, KeyForPadAxis(list, count, static_cast< PADAxis >(index)), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kPadButton: {
    const PADButtonMapping* list = PADGetButtonMappings(kControlPort, &count);
    const u32 native = NativeButtonForPadButton(list, count, kControlPadButtons[index].button);
    return {SInput::kPadButton, static_cast< s32 >(native), AXIS_SIGN_POSITIVE};
  }
  case ECapture::kPadAxis: {
    const PADAxisMapping* list = PADGetAxisMappings(kControlPort, &count);
    for (u32 i = 0; list != nullptr && i < count; ++i) {
      if (list[i].padAxis == static_cast< PADAxis >(index)) {
        return {SInput::kPadAxis, list[i].nativeAxis.nativeAxis, list[i].nativeAxis.sign};
      }
    }
    return {SInput::kPadAxis, -1, AXIS_SIGN_POSITIVE};
  }
  case ECapture::kNone:
    break;
  }
  return {};
}

bool SameInput(const SInput& a, const SInput& b) {
  return a.kind == b.kind && a.code == b.code && a.code != -1 &&
         (a.kind != SInput::kPadAxis || a.sign == b.sign);
}

// The keyboard has L and R twice, as the click and the analog trigger, and the
// defaults put both on one key: that pair isn't a conflict, and binding one
// half moves the other along while they still share a key.
bool PairedRow(ECapture kind, int index, ECapture& pairKind, int& pairIndex) {
  if (kind == ECapture::kKeyButton) {
    const PADButton button = kControlPadButtons[index].button;
    pairKind = ECapture::kKeyAxis;
    pairIndex = button == PAD_TRIGGER_L ? PAD_AXIS_TRIGGER_L : button == PAD_TRIGGER_R ? PAD_AXIS_TRIGGER_R : -1;
    return pairIndex >= 0;
  }
  if (kind == ECapture::kKeyAxis && (index == PAD_AXIS_TRIGGER_L || index == PAD_AXIS_TRIGGER_R)) {
    const PADButton button = index == PAD_AXIS_TRIGGER_L ? PAD_TRIGGER_L : PAD_TRIGGER_R;
    for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      if (kControlPadButtons[i].button == button) {
        pairKind = ECapture::kKeyButton;
        pairIndex = i;
        return true;
      }
    }
  }
  return false;
}

// Binds a row's slot, taking its L/R partner's same slot along if the two
// shared the old key.
void BindWithPair(ECapture kind, int index, int slot, const SInput& input) {
  ECapture pairKind = ECapture::kNone;
  int pairIndex = -1;
  const bool paired = PairedRow(kind, index, pairKind, pairIndex) &&
                      SameInput(RowInput(kind, index, slot), RowInput(pairKind, pairIndex, slot));
  BindRow(kind, index, slot, input);
  if (paired) {
    BindRow(pairKind, pairIndex, slot, input);
  }
}

// Another row slot already driven by `input`, other than the row itself (either
// slot) and its L/R partner.
bool FindConflict(ECapture kind, int index, const SInput& input, ECapture& otherKind, int& otherIndex,
                  int& otherSlot) {
  ECapture pairKind = ECapture::kNone;
  int pairIndex = -1;
  PairedRow(kind, index, pairKind, pairIndex);
  const auto check = [&](ECapture rowKind, int rowCount, int slots) {
    for (int i = 0; i < rowCount; ++i) {
      if ((rowKind == kind && i == index) || (rowKind == pairKind && i == pairIndex)) {
        continue;
      }
      for (int s = 0; s < slots; ++s) {
        if (SameInput(RowInput(rowKind, i, s), input)) {
          otherKind = rowKind;
          otherIndex = i;
          otherSlot = s;
          return true;
        }
      }
    }
    return false;
  };
  const int buttonRows = static_cast< int >(std::size(kControlPadButtons));
  switch (input.kind) {
  case SInput::kKey:
    return check(ECapture::kKeyButton, buttonRows, PAD_KEY_SLOT_COUNT) ||
           check(ECapture::kKeyAxis, PAD_AXIS_COUNT, PAD_KEY_SLOT_COUNT);
  case SInput::kPadButton:
    return check(ECapture::kPadButton, buttonRows, 1);
  case SInput::kPadAxis:
    return check(ECapture::kPadAxis, PAD_AXIS_COUNT, 1);
  }
  return false;
}

void PollCapture() {
  if (sCapture.target == ECapture::kNone) {
    return;
  }
  if (sCapture.conflict) {
    if (!sCapture.conflictReleased) {
      sCapture.conflictReleased = !InputHeld(sCapture.bound);
    } else if (SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE]) {
      CancelCapture();
    }
    return;
  }
  if (SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs) {
    CancelCapture();
    return;
  }
  if (sCapture.settling) {
    if (!InputHeld(sCapture.bound)) {
      CancelCapture();
    }
    return;
  }

  SInput input;
  if (!NewInput(input)) {
    return;
  }
  // Esc cancels, so it can't be bound itself; it has no default binding.
  if (input.kind == SInput::kKey && input.code == SDL_SCANCODE_ESCAPE) {
    CancelCapture();
    return;
  }
  // The overlay covers most of the screen, so a mouse button binds when clicked
  // on the Press... button (or over the game); a click anywhere else on the
  // overlay, such as a tab, is aimed at the UI and cancels.
  if (input.kind == SInput::kKey && input.code < 0) {
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool onPress = mouse.x >= sCapture.pressMin.x && mouse.x < sCapture.pressMax.x &&
                         mouse.y >= sCapture.pressMin.y && mouse.y < sCapture.pressMax.y;
    if (!onPress && ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                                           ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
      CancelCapture();
      return;
    }
  }
  sCapture.bound = input;
  if (FindConflict(sCapture.target, sCapture.index, input, sCapture.otherKind, sCapture.otherIndex,
                   sCapture.otherSlot)) {
    sCapture.conflict = true;
    return;
  }
  BindWithPair(sCapture.target, sCapture.index, sCapture.slot, input);
  PADSerializeMappings();
  sCapture.settling = true;
  sCapture.startMs = SDL_GetTicks();
}

bool Listening(ECapture target, int index, int slot) {
  return sCapture.target == target && sCapture.index == index && sCapture.slot == slot && !sCapture.settling &&
         !sCapture.conflict;
}

std::string ScancodeName(s32 scancode) {
  switch (scancode) {
  case PAD_KEY_INVALID:
    return "(unbound)";
  case PAD_KEY_MOUSE_LEFT:
    return "Mouse Left";
  case PAD_KEY_MOUSE_MIDDLE:
    return "Mouse Middle";
  case PAD_KEY_MOUSE_RIGHT:
    return "Mouse Right";
  case PAD_KEY_MOUSE_X1:
    return "Mouse X1";
  case PAD_KEY_MOUSE_X2:
    return "Mouse X2";
  default:
    break;
  }
  const char* name = SDL_GetScancodeName(static_cast< SDL_Scancode >(scancode));
  return name != nullptr && name[0] != '\0' ? name : "(unknown)";
}

s32 KeyForPadButton(const PADKeyButtonBinding* list, u32 count, PADButton button) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padButton == button) {
      return list[i].scancode;
    }
  }
  return PAD_KEY_INVALID;
}

s32 KeyForPadAxis(const PADKeyAxisBinding* list, u32 count, PADAxis axis) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padAxis == axis) {
      return list[i].scancode;
    }
  }
  return PAD_KEY_INVALID;
}

u32 NativeButtonForPadButton(const PADButtonMapping* list, u32 count, PADButton button) {
  for (u32 i = 0; list != nullptr && i < count; ++i) {
    if (list[i].padButton == button) {
      return list[i].nativeButton;
    }
  }
  return PAD_NATIVE_BUTTON_INVALID;
}

// PADGetNativeAxisName leaves out the direction, which matters here: each
// stick axis is bound one half at a time.
std::string NativeAxisName(const PADSignedNativeAxis& axis) {
  if (axis.nativeAxis < 0) {
    return "(unbound)";
  }
  const char* name = PADGetNativeAxisName(axis);
  std::string result = name != nullptr ? name : "(axis)";
  if (axis.nativeAxis != SDL_GAMEPAD_AXIS_LEFT_TRIGGER && axis.nativeAxis != SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
    result += axis.sign == AXIS_SIGN_NEGATIVE ? " -" : " +";
  }
  return result;
}

std::string InputName(const SInput& input) {
  switch (input.kind) {
  case SInput::kKey:
    return ScancodeName(input.code);
  case SInput::kPadButton: {
    if (input.code == -1) {
      return "(unbound)";
    }
    const char* name = PADGetNativeButtonName(static_cast< u32 >(input.code));
    return name != nullptr ? name : "(unknown)";
  }
  case SInput::kPadAxis:
    return NativeAxisName({input.code, input.sign});
  }
  return "(unknown)";
}

// "Fire / Bomb (A)", or "the alt key of Fire / Bomb (A)" for a keyboard row's
// second slot.
std::string RowLabel(ECapture kind, int index, int slot) {
  std::string label = kind == ECapture::kKeyAxis || kind == ECapture::kPadAxis
                          ? ActionLabel(kControlPadAxes[index].function, kControlPadAxes[index].label)
                          : ActionLabel(kControlPadButtons[index].function, kControlPadButtons[index].label);
  return slot != 0 ? "the alt key of " + label : label;
}

// The swap / bind both / cancel prompt for a captured input another row uses.
void DrawConflict() {
  const SInput old = RowInput(sCapture.target, sCapture.index, sCapture.slot);
  const std::string other = RowLabel(sCapture.otherKind, sCapture.otherIndex, sCapture.otherSlot);
  ImGui::Text("%s is already bound to %s.", InputName(sCapture.bound).c_str(), other.c_str());
  ImGui::BeginDisabled(!sCapture.conflictReleased);
  // Aurora can't leave a controller axis unbound, so an axis row with nothing
  // to hand over can't swap.
  ImGui::BeginDisabled(sCapture.target == ECapture::kPadAxis && old.code == -1);
  const std::string swapLabel =
      (old.code == -1 ? "Move: " + other + " loses it" : "Swap: " + other + " gets " + InputName(old)) + "###swap";
  if (ImGui::Button(swapLabel.c_str())) {
    BindWithPair(sCapture.target, sCapture.index, sCapture.slot, sCapture.bound);
    BindWithPair(sCapture.otherKind, sCapture.otherIndex, sCapture.otherSlot, old);
    PADSerializeMappings();
    CancelCapture();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Bind both")) {
    BindWithPair(sCapture.target, sCapture.index, sCapture.slot, sCapture.bound);
    PADSerializeMappings();
    CancelCapture();
  }
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) {
    CancelCapture();
  }
  ImGui::EndDisabled();
}

// A row slot's binding, as a button that starts that slot's capture.
void BindingButton(ECapture target, int index, int slot, const std::string& name, float width) {
  const bool listening = Listening(target, index, slot);
  const std::string label = (listening ? std::string("Press...") : name) + "###bind" + std::to_string(slot);
  if (ImGui::Button(label.c_str(), ImVec2(width, 0.f))) {
    StartCapture(target, index, slot);
  }
  if (listening) {
    sCapture.pressMin = ImGui::GetItemRectMin();
    sCapture.pressMax = ImGui::GetItemRectMax();
  }
}

// A keyboard row: the action, then each slot's key with a button to clear it.
void KeyRow(ECapture kind, int index, const std::string& label, const float* slotX, float keyWidth) {
  const float rowX = ImGui::GetCursorPosX();
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(label.c_str());
  for (int slot = 0; slot < PAD_KEY_SLOT_COUNT; ++slot) {
    ImGui::PushID(slot);
    const SInput input = RowInput(kind, index, slot);
    ImGui::SameLine(rowX + slotX[slot]);
    BindingButton(kind, index, slot, input.code == PAD_KEY_INVALID ? std::string("-") : InputName(input), keyWidth);
    ImGui::SameLine(0.f, ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::BeginDisabled(input.code == PAD_KEY_INVALID);
    if (ImGui::Button("x")) {
      BindWithPair(kind, index, slot, SInput{});
      PADSerializeMappings();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
      ImGui::SetTooltip("Clear");
    }
    ImGui::EndDisabled();
    ImGui::PopID();
  }
}

} // namespace

namespace PortControls {

void ApplyDefaultKeyBindings(unsigned port) {
  PADKeyButtonBinding buttons[PAD_BUTTON_COUNT] = {
      {SDL_SCANCODE_X, PAD_BUTTON_A},          {SDL_SCANCODE_Z, PAD_BUTTON_B},
      {SDL_SCANCODE_C, PAD_BUTTON_X},          {SDL_SCANCODE_V, PAD_BUTTON_Y},
      {SDL_SCANCODE_RETURN, PAD_BUTTON_START}, {SDL_SCANCODE_F, PAD_TRIGGER_Z},
      {SDL_SCANCODE_Q, PAD_TRIGGER_L},         {SDL_SCANCODE_E, PAD_TRIGGER_R},
      {SDL_SCANCODE_UP, PAD_BUTTON_UP},        {SDL_SCANCODE_DOWN, PAD_BUTTON_DOWN},
      {SDL_SCANCODE_LEFT, PAD_BUTTON_LEFT},    {SDL_SCANCODE_RIGHT, PAD_BUTTON_RIGHT},
  };
  PADKeyAxisBinding axes[PAD_AXIS_COUNT] = {
      {SDL_SCANCODE_D, PAD_AXIS_LEFT_X_POS, 1},  {SDL_SCANCODE_A, PAD_AXIS_LEFT_X_NEG, 1},
      {SDL_SCANCODE_W, PAD_AXIS_LEFT_Y_POS, 1},  {SDL_SCANCODE_S, PAD_AXIS_LEFT_Y_NEG, 1},
      {SDL_SCANCODE_L, PAD_AXIS_RIGHT_X_POS, 1}, {SDL_SCANCODE_J, PAD_AXIS_RIGHT_X_NEG, 1},
      {SDL_SCANCODE_I, PAD_AXIS_RIGHT_Y_POS, 1}, {SDL_SCANCODE_K, PAD_AXIS_RIGHT_Y_NEG, 1},
      {SDL_SCANCODE_Q, PAD_AXIS_TRIGGER_L, 1},   {SDL_SCANCODE_E, PAD_AXIS_TRIGGER_R, 1},
  };
  if (PADSetKeyButtonBindings(port, buttons) && PADSetKeyAxisBindings(port, axes)) {
    PADSetKeyboardActive(port, TRUE);
  }
  // The defaults have no alt keys.
  for (const PADKeyButtonBinding& binding : buttons) {
    PADSetKeyButtonBindingSlot(port, 1, {PAD_KEY_INVALID, binding.padButton});
  }
  for (const PADKeyAxisBinding& binding : axes) {
    PADSetKeyAxisBindingSlot(port, 1, {PAD_KEY_INVALID, binding.padAxis, 1});
  }
}

bool Capturing() {
  // Only the tab polls the capture, so one left running when the overlay closed
  // or the tab changed would otherwise block the overlay's pad navigation.
  if (sCapture.target != ECapture::kNone &&
      (ImGui::GetFrameCount() - sLastDrawFrame > 2 ||
       (!sCapture.conflict && SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs))) {
    CancelCapture();
  }
  // A conflict prompt is answered with the pad too, once the captured input
  // is released.
  return sCapture.target != ECapture::kNone && !(sCapture.conflict && sCapture.conflictReleased);
}

void DrawTab() {
  sLastDrawFrame = ImGui::GetFrameCount();
  PollCapture();
  // Wide enough for the usual key names, so the columns line up at any font
  // scale; a longer name is clipped.
  const float bindWidth =
      std::max(ImGui::CalcTextSize("Mouse Middle").x, ImGui::CalcTextSize("Press...").x) +
      ImGui::GetStyle().FramePadding.x * 2.f;

  // A modal rather than an inline prompt: when an inline one closed, the rows
  // below moved up under the cursor, so a double-click on Swap could land on
  // Restore keyboard defaults.
  constexpr const char* kConflictPopup = "Binding conflict";
  if (sCapture.conflict && !ImGui::IsPopupOpen(kConflictPopup)) {
    ImGui::OpenPopup(kConflictPopup);
  }
  const ImVec2 overlayCenter(ImGui::GetWindowPos().x + ImGui::GetWindowWidth() * 0.5f,
                             ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f);
  ImGui::SetNextWindowPos(overlayCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal(kConflictPopup, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (sCapture.conflict) {
      DrawConflict();
    }
    if (!sCapture.conflict) {
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }

  if (sCapture.target != ECapture::kNone && !sCapture.settling && !sCapture.conflict) {
    const bool keys =
        sCapture.target == ECapture::kKeyButton || sCapture.target == ECapture::kKeyAxis;
    const Uint64 elapsed = SDL_GetTicks() - sCapture.startMs;
    const unsigned left =
        static_cast< unsigned >((kCaptureTimeoutMs - std::min(elapsed, kCaptureTimeoutMs) + 999) / 1000);
    ImGui::Text("%s (Esc cancels, %us)",
                keys ? "Press a key, or click Press... with a mouse button" : "Press a controller button or stick...",
                left);
    if (!keys) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Cancel")) {
        CancelCapture();
      }
    }
  } else {
    ImGui::TextUnformatted("Pad 1. Click a binding, then press the input to assign it.");
  }

  // Nothing else is clickable while an input is being captured: the capture
  // owns every key, button and click until it binds or is cancelled.
  ImGui::BeginDisabled(sCapture.target != ECapture::kNone);
  if (ImGui::Button("Restore keyboard defaults")) {
    ApplyDefaultKeyBindings(kControlPort);
    PADSerializeMappings();
  }
  ImGui::SameLine();
  if (ImGui::Button("Restore controller defaults")) {
    PADRestoreDefaultMapping(kControlPort);
    PADSerializeMappings();
  }

  std::string buttonLabels[std::size(kControlPadButtons)];
  std::string axisLabels[PAD_AXIS_COUNT];
  float labelWidth = 0.f;
  for (size_t i = 0; i < std::size(kControlPadButtons); ++i) {
    buttonLabels[i] = ActionLabel(kControlPadButtons[i].function, kControlPadButtons[i].label);
    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(buttonLabels[i].c_str()).x);
  }
  for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
    axisLabels[i] = ActionLabel(kControlPadAxes[i].function, kControlPadAxes[i].label);
    labelWidth = std::max(labelWidth, ImGui::CalcTextSize(axisLabels[i].c_str()).x);
  }

  // Columns: the action, then each key slot (a binding button and its clear
  // button). The controller's single binding spans the key column.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float clearWidth = ImGui::CalcTextSize("x").x + style.FramePadding.x * 2.f;
  const float slotX[PAD_KEY_SLOT_COUNT] = {
      labelWidth + style.ItemSpacing.x * 2.f,
      labelWidth + style.ItemSpacing.x * 4.f + bindWidth + style.ItemInnerSpacing.x + clearWidth,
  };

  if (ImGui::CollapsingHeader("Keyboard & mouse", ImGuiTreeNodeFlags_DefaultOpen)) {
    const float rowX = ImGui::GetCursorPosX();
    ImGui::TextDisabled("Action");
    ImGui::SameLine(rowX + slotX[0]);
    ImGui::TextDisabled("Key");
    ImGui::SameLine(rowX + slotX[1]);
    ImGui::TextDisabled("Alt key");
    for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      ImGui::PushID(i);
      KeyRow(ECapture::kKeyButton, i, buttonLabels[i], slotX, bindWidth);
      ImGui::PopID();
    }
    for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
      ImGui::PushID(100 + i);
      KeyRow(ECapture::kKeyAxis, i, axisLabels[i], slotX, bindWidth);
      ImGui::PopID();
    }
  }

  if (ImGui::CollapsingHeader("Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
    u32 padButtonCount = 0;
    if (PADGetButtonMappings(kControlPort, &padButtonCount) == nullptr) {
      ImGui::TextDisabled("No controller on pad 1.");
    } else {
      const float padWidth = bindWidth + style.ItemInnerSpacing.x + clearWidth;
      const auto padRow = [&](ECapture kind, int index, const std::string& label) {
        const float rowX = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine(rowX + slotX[0]);
        BindingButton(kind, index, 0, InputName(RowInput(kind, index, 0)), padWidth);
      };
      for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
        ImGui::PushID(200 + i);
        padRow(ECapture::kPadButton, i, buttonLabels[i]);
        ImGui::PopID();
      }
      for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
        ImGui::PushID(300 + i);
        padRow(ECapture::kPadAxis, i, axisLabels[i]);
        ImGui::PopID();
      }
    }
  }
  ImGui::EndDisabled();
}

} // namespace PortControls
