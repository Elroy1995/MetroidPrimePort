#include "Kyoto/Input/CDolphinController.hpp"
#include "port_console.h"
#include "port_debug.h"
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#ifdef MP_ENABLE_SMOKE_DRIVER
#include "port_smoke.h"
#endif

#include <Kyoto/Alloc/CMemory.hpp>

#include <dolphin/gba.h>
#include <dolphin/os/OSSerial.h>

#ifdef TARGET_PC
#include "port_controls.h"
#endif

#include <string.h>

CDolphinController::CDolphinController()
: x1c4_validControllers(PAD_CHAN0_BIT | PAD_CHAN1_BIT | PAD_CHAN2_BIT | PAD_CHAN3_BIT)
, x1c8_invalidControllers(0)
, x1cc_(0) {
  static bool sIsInitialized = false;
  if (!sIsInitialized) {
    PADSetSpec(PAD_SPEC_5);
    PADInit();
#ifdef TARGET_PC
    u32 bindingCount = 0;
    if (PADGetKeyButtonBindings(PAD_CHAN0, &bindingCount) == nullptr) {
      PortControls::ApplyDefaultKeyBindings(PAD_CHAN0);
    }
#endif
    sIsInitialized = true;
  }
}

CDolphinController::~CDolphinController() {}

bool CDolphinController::Initialize() {
  GBAInit();
  memset(x4_status, 0, sizeof(PADStatus) * 4);
  for (uint i = 0; i < 4; ++i) {
    x34_gamepadStates[i].SetDeviceIsPresent(false);
    x194_motorStates[i] = kMS_StopHard;
    x1b4_controllerTypePollTime[i] = 0;
    x1a4_controllerTypes[i] = skTypeUnknown;
  }

  PADControlAllMotors((const u32*)x194_motorStates);
  Poll();
  return true;
}

void CDolphinController::Poll() {
  ReadDevices();
  ProcessInputData();
}

void CDolphinController::ReadDevices() {
  PADStatus status[4]{};
  PADRead(status);
  PADClamp(status);
  // Not SDL_GetMouseState: that counts touches as left clicks.
  unsigned held = PortDebug::MouseHeldButtons() &
                  (SDL_BUTTON_LMASK | SDL_BUTTON_RMASK | SDL_BUTTON_MMASK);
  bool inputFocused = SDL_GetKeyboardFocus() != nullptr;
#ifdef MP_ENABLE_SMOKE_DRIVER
  held = PortSmokeMouseButtons(held);
  inputFocused = inputFocused || PortSmokeMouseEnabled() || PortSmokeScriptedInput();
#endif
  // The console's pad commands work in a window without focus.
  inputFocused = inputFocused || PortConsoleEnabled();
  const unsigned mouse = PortDebug::MouseWeaponButtons(held);
  // Out of first-person aim a left click is still A: bombs in morph ball, and
  // advancing text boxes and menus. Its gate also waits for a release, so a
  // held charge carried into morph ball does not drop a bomb.
  if (PortDebug::MouseMenuButtons(held, inputFocused) != 0) {
    status[0].err = PAD_ERR_NONE;
    status[0].button |= PAD_BUTTON_A;
  }
  if (PortDebug::MouseGameplayActive() && PortDebug::MouseCaptured() && PortDebug::MouseButtons()) {
    // Add held states to the normal PAD path: its press/release edges drive
    // charge shots and missile cooldowns. Saved bindings remain untouched.
    status[0].err = PAD_ERR_NONE;
    if (mouse & SDL_BUTTON_LMASK) status[0].button |= PAD_BUTTON_A;
    if (mouse & SDL_BUTTON_MMASK) status[0].button |= PAD_BUTTON_Y;
    if (mouse & SDL_BUTTON_RMASK) {
      status[0].button |= PAD_TRIGGER_L;
      status[0].triggerL = 150; // fully depressed, after PADClamp's dead zone
    }
  }
  for (int i = 0; i < 4; ++i) {
    // One disconnected port must not prevent the other ports updating. Clear
    // stale held buttons on disconnect and keep UI interaction out of gameplay.
    if (status[i].err != PAD_ERR_NONE || PortDebug::Visible() || !inputFocused) {
      const auto error = status[i].err;
      status[i] = {};
      status[i].err = error;
    }
  }
  memcpy(x4_status, status, sizeof(status));

  // Twin-stick: feed the right stick into the first-person aim and consume it,
  // so it does not also drive the game's own free-look.
  if (PortDebug::TwinStick() && x4_status[0].err == PAD_ERR_NONE) {
    const float sx = static_cast< float >(x4_status[0].substickX) / 127.f;
    const float sy = static_cast< float >(x4_status[0].substickY) / 127.f;
    PortDebug::AddStickAim(sx, sy, PortDebug::TickPeriod());
    PortDebug::SetTwinStickRightY(sy);
    x4_status[0].substickX = 0;
    x4_status[0].substickY = 0;

    // Beams are selected from the C-stick, which twin-stick just consumed, so
    // holding a modifier lets the D-pad stand in for it: the four directions
    // select beams instead of switching visors. The L trigger works on a pad;
    // the Android overlay's RB sends left shift, which also gives a keyboard
    // binding on desktop. Directions must match the C-stick's (positive X is
    // right, positive Y is up) so each D-pad direction picks the same beam that
    // direction on the C-stick would.
    const bool* keys = SDL_GetKeyboardState(nullptr);
    SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0);
    const bool beamModifier =
        (x4_status[0].button & PAD_TRIGGER_L) != 0 ||
        (keys != nullptr && keys[SDL_SCANCODE_LSHIFT] != 0) ||
        (pad != nullptr && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    if (beamModifier) {
      const int x = ((x4_status[0].button & PAD_BUTTON_RIGHT) != 0 ? 127 : 0) -
                    ((x4_status[0].button & PAD_BUTTON_LEFT) != 0 ? 127 : 0);
      const int y = ((x4_status[0].button & PAD_BUTTON_UP) != 0 ? 127 : 0) -
                    ((x4_status[0].button & PAD_BUTTON_DOWN) != 0 ? 127 : 0);
      x4_status[0].substickX = static_cast< s8 >(x);
      x4_status[0].substickY = static_cast< s8 >(y);
      x4_status[0].button &= ~(PAD_BUTTON_UP | PAD_BUTTON_DOWN | PAD_BUTTON_LEFT | PAD_BUTTON_RIGHT);
    }
  } else {
    PortDebug::SetTwinStickRightY(0.f);
  }

  // Start+Back is the debug overlay chord; do not also pause the game with it.
  if (SDL_Gamepad* pad = PADGetSDLGamepadForIndex(0)) {
    if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK)) {
      x4_status[0].button &= ~PAD_BUTTON_START;
    }
  }

  for (int i = 0; i < 4; ++i) {
    uint controller = (PAD_CHAN0_BIT >> i);
    if (x4_status[i].err != PAD_ERR_NOT_READY) {
      if (x4_status[i].err == PAD_ERR_NONE) {
        x34_gamepadStates[i].SetDeviceIsPresent(true);
      } else if (x4_status[i].err == PAD_ERR_NO_CONTROLLER) {
        x1c8_invalidControllers |= controller;
        x34_gamepadStates[i].SetDeviceIsPresent(false);
      }
    }

    if (x1b4_controllerTypePollTime[i] != 0) {
      --x1b4_controllerTypePollTime[i];
    } else {
      const uint type = SIProbe(i);
      if ((type & (SI_ERROR_NO_RESPONSE | SI_ERROR_UNKNOWN | SI_ERROR_BUSY)) != 0) {
        if (x1b4_controllerTypePollTime[i] == 0) {
          x1a4_controllerTypes[i] = skTypeUnknown;
        }
      } else {
        x1b4_controllerTypePollTime[i] = 60;
        if (type == SI_GC_WAVEBIRD) {
          x1a4_controllerTypes[i] = skTypeWavebird;
        } else if (type == SI_GBA) {
          x1a4_controllerTypes[i] = skTypeGBA;
        } else if (type == SI_GC_CONTROLLER) {
          x1a4_controllerTypes[i] = skTypeStandard;
        }
      }
    }
  }

  if (x1c8_invalidControllers != 0 && PADReset(x1c8_invalidControllers)) {
    x1c8_invalidControllers = 0;
  }
}

void CDolphinController::ProcessInputData() {
  for (int i = 0; i < 4; ++i) {
    ProcessAxis(i, kJA_LeftX);
    ProcessAxis(i, kJA_LeftY);
    ProcessAxis(i, kJA_RightX);
    ProcessAxis(i, kJA_RightY);
    ProcessButtons(i);
  }
}

void CDolphinController::ProcessAxis(int controller, EJoyAxis axis) {
  const float maxAxisValue = 1.f / GetAnalogStickMaxValue(axis);
  CControllerAxis& data = x34_gamepadStates[controller].GetAxis(axis);

  float axisValue = 0.f;
  switch (axis) {
  case kJA_LeftX:
    axisValue = x4_status[controller].stickX;
    break;
  case kJA_LeftY:
    axisValue = x4_status[controller].stickY;
    break;
  case kJA_RightX:
    axisValue = x4_status[controller].substickX;
    break;
  case kJA_RightY:
    axisValue = x4_status[controller].substickY;
    break;
  default:
    break;
  }

  float absolute = axisValue * maxAxisValue;
  if (absolute < kAbsoluteMinimum) {
    absolute = kAbsoluteMinimum;
  } else if (absolute > kAbsoluteMaximum) {
    absolute = kAbsoluteMaximum;
  }

  float relativeValue = absolute - data.GetAbsoluteValue();
  if (relativeValue < kRelativeMinimum) {
    relativeValue = kRelativeMinimum;
  } else if (relativeValue > kRelativeMaximum) {
    relativeValue = kRelativeMaximum;
  }
  data.SetRelativeValue(relativeValue);
  data.SetAbsoluteValue(absolute);
}

static ushort mButtonMapping[size_t(kBU_MAX)] = {
    PAD_BUTTON_A,     PAD_BUTTON_B,    PAD_BUTTON_X,  PAD_BUTTON_Y,
    PAD_BUTTON_START, PAD_TRIGGER_Z,   PAD_BUTTON_UP, PAD_BUTTON_RIGHT,
    PAD_BUTTON_DOWN,  PAD_BUTTON_LEFT, PAD_TRIGGER_L, PAD_TRIGGER_R,
};

void CDolphinController::ProcessButtons(int controller) {
  for (int i = 0; i < int(kBU_MAX); ++i) {
    ProcessDigitalButton(controller, x34_gamepadStates[controller].GetButton(EButton(i)),
                         mButtonMapping[i]);
  }

  ProcessAnalogButton(x4_status[controller].triggerL,
                      x34_gamepadStates[controller].GetAnalogButton(kBA_Left));
  ProcessAnalogButton(x4_status[controller].triggerR,
                      x34_gamepadStates[controller].GetAnalogButton(kBA_Right));
}

void CDolphinController::ProcessDigitalButton(int controller, CControllerButton& button,
                                              ushort mapping) {
  bool btnPressed = (x4_status[controller].button & mapping);
  button.SetPressEvent(PADButtonDown(button.GetIsPressed(), btnPressed));
  button.SetReleaseEvent(PADButtonUp(button.GetIsPressed(), btnPressed));
  button.SetIsPressed(btnPressed);
}

void CDolphinController::ProcessAnalogButton(float value, CControllerAxis& axis) {
  value *= 1.f / 150.f;
  if (value > kAbsoluteMaximum) {
    value = kAbsoluteMaximum;
  }

  float relative = value - axis.GetAbsoluteValue();
  if (relative > kRelativeMaximum) {
    relative = kRelativeMaximum;
  }

  axis.SetRelativeValue(relative);
  axis.SetAbsoluteValue(value);
}

uint CDolphinController::GetDeviceCount() const { return 4; }

CControllerGamepadData& CDolphinController::GetGamepadData(int controller) {
  return x34_gamepadStates[controller];
}

uint CDolphinController::GetControllerType(int controller) const {
  return x1a4_controllerTypes[controller];
}

void CDolphinController::SetMotorState(EIOPort port, EMotorState state) {
  x194_motorStates[port] = state;
  PADControlAllMotors((const u32*)x194_motorStates);
}

float CDolphinController::GetAnalogStickMaxValue(EJoyAxis axis) const {
  switch (axis) {
  case kJA_LeftX:
  case kJA_LeftY:
    return 72.0f;

  case kJA_RightX:
  case kJA_RightY:
    return 59.0f;

  default:
    return 0.0f;
  }
}

const uint CDolphinController::skTypeUnknown = 'UNKN';
const uint CDolphinController::skTypeStandard = 'STND';
const uint CDolphinController::skTypeGBA = 'GBA_';
const uint CDolphinController::skTypeWavebird = 'WAVE';
