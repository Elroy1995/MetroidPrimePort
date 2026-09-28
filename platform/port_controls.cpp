// Controls tab: rebinds keyboard/mouse and controller inputs to the emulated
// pad. The binding backend (matching, persistence, name helpers) is Aurora's.

#include "port_controls.h"
#include "port_debug.h"

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

struct SControlPadButton {
  PADButton button;
  const char* label;
};
const SControlPadButton kControlPadButtons[] = {
    {PAD_BUTTON_A, "A"},             {PAD_BUTTON_B, "B"},
    {PAD_BUTTON_X, "X"},             {PAD_BUTTON_Y, "Y"},
    {PAD_TRIGGER_L, "L"},            {PAD_TRIGGER_R, "R"},
    {PAD_TRIGGER_Z, "Z"},            {PAD_BUTTON_START, "Start"},
    {PAD_BUTTON_UP, "D-pad Up"},     {PAD_BUTTON_DOWN, "D-pad Down"},
    {PAD_BUTTON_LEFT, "D-pad Left"}, {PAD_BUTTON_RIGHT, "D-pad Right"},
};

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

void StartCapture(ECapture target, int index) {
  sCapture = SCapture{};
  sCapture.target = target;
  sCapture.index = index;
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

void Bind(const SInput& input) {
  const int index = sCapture.index;
  switch (sCapture.target) {
  case ECapture::kKeyButton: {
    PADKeyButtonBinding binding{};
    binding.scancode = input.code;
    binding.padButton = kControlPadButtons[index].button;
    PADSetKeyButtonBinding(kControlPort, binding);
    // A binding on a switched-off keyboard would never fire.
    PADSetKeyboardActive(kControlPort, TRUE);
    break;
  }
  case ECapture::kKeyAxis: {
    PADKeyAxisBinding binding{};
    binding.scancode = input.code;
    binding.padAxis = static_cast< PADAxis >(index);
    binding.influence = 1;
    PADSetKeyAxisBinding(kControlPort, binding);
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
    return;
  }
  PADSerializeMappings();
}

void PollCapture() {
  if (sCapture.target == ECapture::kNone) {
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
  Bind(input);
  sCapture.settling = true;
  sCapture.bound = input;
  sCapture.startMs = SDL_GetTicks();
}

bool Listening(ECapture target, int index) {
  return sCapture.target == target && sCapture.index == index && !sCapture.settling;
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

std::string PadAxisName(PADAxis axis) {
  const char* name = PADGetAxisName(axis);
  const char* dir = PADGetAxisDirectionLabel(axis);
  if (name == nullptr) {
    return "(axis)";
  }
  return dir != nullptr ? std::string(name) + " " + dir : std::string(name);
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

// The Bind button of one row; starts that row's capture when clicked.
void BindButton(ECapture target, int index, float width) {
  if (ImGui::Button(Listening(target, index) ? "Press..." : "Bind", ImVec2(width, 0.f))) {
    StartCapture(target, index);
  }
  if (Listening(target, index)) {
    sCapture.pressMin = ImGui::GetItemRectMin();
    sCapture.pressMax = ImGui::GetItemRectMax();
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
}

bool Capturing() {
  // Only the tab polls the capture, so one left running when the overlay closed
  // or the tab changed would otherwise block the overlay's pad navigation.
  if (sCapture.target != ECapture::kNone &&
      (ImGui::GetFrameCount() - sLastDrawFrame > 2 || SDL_GetTicks() - sCapture.startMs > kCaptureTimeoutMs)) {
    CancelCapture();
  }
  return sCapture.target != ECapture::kNone;
}

void DrawTab() {
  sLastDrawFrame = ImGui::GetFrameCount();
  PollCapture();
  // Wide enough for the longer label, so the rows line up at any font scale.
  const float bindWidth =
      ImGui::CalcTextSize("Press...").x + ImGui::GetStyle().FramePadding.x * 2.f;

  if (sCapture.target != ECapture::kNone && !sCapture.settling) {
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
    ImGui::TextUnformatted("Pad 1. Click Bind, then press the input to assign it.");
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

  u32 keyButtonCount = 0;
  PADKeyButtonBinding* keyButtons = PADGetKeyButtonBindings(kControlPort, &keyButtonCount);
  u32 keyAxisCount = 0;
  PADKeyAxisBinding* keyAxes = PADGetKeyAxisBindings(kControlPort, &keyAxisCount);
  u32 padButtonCount = 0;
  PADButtonMapping* padButtons = PADGetButtonMappings(kControlPort, &padButtonCount);
  u32 padAxisCount = 0;
  PADAxisMapping* padAxes = PADGetAxisMappings(kControlPort, &padAxisCount);

  if (ImGui::CollapsingHeader("Keyboard & mouse", ImGuiTreeNodeFlags_DefaultOpen)) {
    for (int i = 0; i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      ImGui::PushID(i);
      const PADButton button = kControlPadButtons[i].button;
      BindButton(ECapture::kKeyButton, i, bindWidth);
      ImGui::SameLine();
      if (ImGui::Button("Clear")) {
        PADKeyButtonBinding binding{};
        binding.scancode = PAD_KEY_INVALID;
        binding.padButton = button;
        PADSetKeyButtonBinding(kControlPort, binding);
        PADSerializeMappings();
      }
      ImGui::SameLine();
      ImGui::Text("%-12s %s", kControlPadButtons[i].label,
                  ScancodeName(KeyForPadButton(keyButtons, keyButtonCount, button)).c_str());
      ImGui::PopID();
    }
    for (int i = 0; i < PAD_AXIS_COUNT; ++i) {
      ImGui::PushID(100 + i);
      BindButton(ECapture::kKeyAxis, i, bindWidth);
      ImGui::SameLine();
      if (ImGui::Button("Clear")) {
        PADKeyAxisBinding binding{};
        binding.scancode = PAD_KEY_INVALID;
        binding.padAxis = static_cast< PADAxis >(i);
        binding.influence = 1;
        PADSetKeyAxisBinding(kControlPort, binding);
        PADSerializeMappings();
      }
      ImGui::SameLine();
      ImGui::Text("%-12s %s", PadAxisName(static_cast< PADAxis >(i)).c_str(),
                  ScancodeName(KeyForPadAxis(keyAxes, keyAxisCount, static_cast< PADAxis >(i))).c_str());
      ImGui::PopID();
    }
  }

  if (ImGui::CollapsingHeader("Controller", ImGuiTreeNodeFlags_DefaultOpen)) {
    if (padButtons == nullptr) {
      ImGui::TextDisabled("No controller on pad 1.");
    }
    for (int i = 0; padButtons != nullptr && i < static_cast< int >(std::size(kControlPadButtons)); ++i) {
      ImGui::PushID(200 + i);
      const PADButton button = kControlPadButtons[i].button;
      BindButton(ECapture::kPadButton, i, bindWidth);
      ImGui::SameLine();
      const u32 native = NativeButtonForPadButton(padButtons, padButtonCount, button);
      const char* nativeName =
          native == PAD_NATIVE_BUTTON_INVALID ? "(unbound)" : PADGetNativeButtonName(native);
      ImGui::Text("%-12s %s", kControlPadButtons[i].label,
                  nativeName != nullptr ? nativeName : "(unknown)");
      ImGui::PopID();
    }
    for (int i = 0; padAxes != nullptr && i < PAD_AXIS_COUNT; ++i) {
      ImGui::PushID(300 + i);
      BindButton(ECapture::kPadAxis, i, bindWidth);
      ImGui::SameLine();
      const char* nativeName = "(unbound)";
      for (u32 j = 0; j < padAxisCount; ++j) {
        if (padAxes[j].padAxis == static_cast< PADAxis >(i)) {
          const char* axisName = PADGetNativeAxisName(padAxes[j].nativeAxis);
          nativeName = axisName != nullptr ? axisName : "(axis)";
          break;
        }
      }
      ImGui::Text("%-12s %s", PadAxisName(static_cast< PADAxis >(i)).c_str(), nativeName);
      ImGui::PopID();
    }
  }
  ImGui::EndDisabled();
}

} // namespace PortControls
