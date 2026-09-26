#include "touch_pad.h"

#include <cmath>

namespace PortTouchPad {

void Describe(SDL_VirtualJoystickDesc& desc) {
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  // Report as a common Xbox pad so the pad type, and with it the port's prompt
  // icons, match what the overlay draws. These ids are cosmetic: a virtual
  // device's GUID uses the virtual bus, so they do not select a built-in
  // mapping. See the header for why no mapping string is installed.
  desc.vendor_id = 0x045e;
  desc.product_id = 0x02ea;
  desc.name = "Metroid Prime touch gamepad";
}

Sint16 AxisValue(float deflection) {
  const float clamped = deflection < -1.f ? -1.f : (deflection > 1.f ? 1.f : deflection);
  const float scaled = clamped < 0.f ? clamped * 32768.f : clamped * 32767.f;
  return static_cast< Sint16 >(std::lround(scaled));
}

Pad Attach() {
  Pad pad;
  SDL_VirtualJoystickDesc desc;
  Describe(desc);
  pad.id = SDL_AttachVirtualJoystick(&desc);
  if (pad.id == 0)
    return pad;

  // No SDL_AddGamepadMapping here on purpose; the header explains why.
  pad.handle = SDL_OpenJoystick(pad.id);
  if (pad.handle == nullptr) {
    // See the header: dropping the id would leak a device per call.
    SDL_DetachVirtualJoystick(pad.id);
    pad.id = 0;
  }
  return pad;
}

void Detach(Pad& pad) {
  if (pad.handle != nullptr) {
    SDL_CloseJoystick(pad.handle);
    pad.handle = nullptr;
  }
  if (pad.id != 0) {
    SDL_DetachVirtualJoystick(pad.id);
    pad.id = 0;
  }
}

} // namespace PortTouchPad
