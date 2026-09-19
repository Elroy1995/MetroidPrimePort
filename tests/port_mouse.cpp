#include "port_mouse.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void Check(bool condition) {
  if (!condition) {
    std::fputs("mouse aim regression failed\n", stderr);
    std::abort();
  }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.00001f; }
}

int main() {
  PortMouse::AimState aim;
  constexpr float sensitivity = 0.0035f;
  // Right/up mouse motion must turn right/up in world coordinates, immediately.
  Check(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, -100.f, sensitivity, false, false));
  Check(Near(aim.yaw, -0.35f) && Near(aim.pitch, 0.35f));
  aim.Reset();
  Check(aim.Update(true, false, 0.f, 1.f, 0.f, 100.f, 100.f, sensitivity, true, true));
  Check(Near(aim.yaw, 0.35f) && Near(aim.pitch, 0.35f));

  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, -2000.f, sensitivity, false, false);
  Check(Near(aim.pitch, PortMouse::kMaxPitch));
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 1.f, sensitivity, false, false);
  Check(Near(aim.pitch, PortMouse::kMaxPitch - sensitivity));
  aim.Update(true, false, 0.f, 1.f, 0.f, 100000.f, 4000.f, sensitivity, false, false);
  Check(std::fabs(aim.yaw) <= PortMouse::kPi && Near(aim.pitch, -PortMouse::kMaxPitch));

  // Target lock updates effective aim but discards hidden motion. Releasing it
  // keeps the camera's last direction, even if the body points elsewhere.
  Check(!aim.Update(true, true, 1.f, 0.f, 0.5f, 500.f, 500.f, sensitivity, false, false));
  Check(Near(aim.yaw, -PortMouse::kPi / 2.f) && Near(aim.pitch, std::atan(0.5f)));
  const float lockedPitch = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f, 0.f, sensitivity, false, false);
  Check(Near(aim.pitch, lockedPitch) && Near(aim.yaw, -PortMouse::kPi / 2.f));
  const float beforePole = aim.yaw;
  aim.Synchronize(0.f, 0.f, 1.f);
  Check(Near(aim.yaw, beforePole) && Near(aim.pitch, PortMouse::kMaxPitch));

  // Cinematic/morph/menu handoff must rebase instead of reusing stale angles.
  Check(!aim.Update(false, false, 0.f, 1.f, 0.f, 500.f, 500.f, sensitivity, false, false));
  Check(!aim.initialized);
  aim.Update(true, false, -1.f, 0.f, -0.5f, 0.f, 0.f, sensitivity, false, false);
  Check(Near(aim.yaw, PortMouse::kPi / 2.f) && Near(aim.pitch, -lockedPitch));
  const float before = aim.pitch;
  aim.Update(true, false, 0.f, 1.f, 0.f, 0.f,
             std::numeric_limits<float>::infinity(), sensitivity, false, false);
  Check(Near(before, aim.pitch));
  aim.Reset();
  Check(!aim.Update(true, false, 0.f, 0.f, 0.f, 1.f, 1.f, sensitivity, false, false));

  PortMouse::ButtonGate buttons;
  Check(buttons.Poll(true, 1) == 0); // click used to enter capture/UI is not fire
  Check(buttons.Poll(true, 1) == 0);
  Check(buttons.Poll(true, 0) == 0);
  for (int tick = 0; tick < 120; ++tick) Check(buttons.Poll(true, 1) == 1);
  Check(buttons.Poll(true, 0) == 0); // charge-release edge remains visible
  Check(buttons.Poll(true, 6) == 6); // simultaneous lock-on/missile buttons
  Check(buttons.Poll(false, 1) == 0);
  Check(buttons.Poll(true, 1) == 0); // do not resume a held click on refocus
  buttons.Poll(true, 0);
  Check(buttons.Poll(true, 1) == 1);
  std::puts("mouse axes, limits, handoff and held-button regressions passed");
}
