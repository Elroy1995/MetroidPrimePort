#include "port_input_map.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "input map regression failed: %s\n", what);
    std::abort();
  }
}
} // namespace

int main() {
  using namespace PortInputMap;

  // Names round-trip, and an unknown one is rejected.
  for (int i = 0; i < kMA_Count; ++i) {
    Check(MouseActionFromName(MouseActionInfo(i).name) == i, "action name round trip");
  }
  Check(MouseActionFromName("fire") == -1, "unknown action name");
  Check(MouseActionInfo(99).padButton == 0 && MouseActionInfo(-1).padButton == 0, "action range");

  // The defaults are the old fixed buttons: left fire, middle missile, right lock-on.
  int actions[kMouseButtonCount];
  for (int i = 0; i < kMouseButtonCount; ++i) {
    actions[i] = DefaultMouseAction(i);
  }
  Check(MouseActions(actions, 1u << 0).buttons == kPadA, "left is A");
  Check(MouseActions(actions, 1u << 1).buttons == kPadY, "middle is Y");
  Check(MouseActions(actions, 1u << 2).buttons == kPadL, "right is L");
  Check(MouseActions(actions, (1u << 3) | (1u << 4)).buttons == 0, "side buttons unbound");
  Check(MouseActions(actions, 0x7).buttons == (kPadA | kPadY | kPadL), "all three");

  // Rebound, with a shift; the menu path lets only its allowed buttons through.
  actions[3] = kMA_B;
  actions[4] = kMA_Shift;
  const SMouseResult both = MouseActions(actions, (1u << 3) | (1u << 4));
  Check(both.buttons == kPadB && both.shift, "side buttons rebound");
  const SMouseResult menu = MouseActions(actions, 0x1f, kPadA | kPadB);
  Check(menu.buttons == (kPadA | kPadB) && !menu.shift, "menu buttons");
  Check(MouseButtonsFor(actions, kPadA | kPadB) == ((1u << 0) | (1u << 3)), "buttons for A and B");
  Check(MouseButtonsFor(actions, 0) == 0, "buttons for nothing");

  // Shift: the D-pad becomes the C-stick and is consumed; a stick is left alone
  // when no direction is pressed.
  unsigned buttons = kPadUp | kPadA;
  int x = 0, y = 0;
  ShiftDPadToCStick(buttons, x, y);
  Check(buttons == kPadA && x == 0 && y == 127, "up");
  buttons = kPadLeft | kPadDown;
  ShiftDPadToCStick(buttons, x, y);
  Check(buttons == 0 && x == -127 && y == -127, "left and down");
  buttons = kPadB;
  x = 40;
  y = -40;
  ShiftDPadToCStick(buttons, x, y);
  Check(buttons == kPadB && x == 40 && y == -40, "no direction");

  // Spring Ball on jump, no Boost Ball: the press springs, once.
  const float dt = 1.f / 60.f;
  SpringTap tap;
  Check(tap.Update(true, false, dt), "press springs");
  Check(!tap.Update(true, false, dt), "hold does not repeat");
  Check(!tap.Update(false, false, dt), "release does nothing");

  // With the Boost Ball: a tap springs on release, a held charge does not.
  Check(!tap.Update(true, true, dt), "press waits");
  Check(!tap.Update(true, true, dt), "still waiting");
  Check(tap.Update(false, true, dt), "tap springs on release");
  Check(!tap.Update(false, true, dt), "idle");
  Check(!tap.Update(true, true, dt), "charge press");
  for (int i = 0; i < 30; ++i) {
    Check(!tap.Update(true, true, dt), "charging");
  }
  Check(!tap.Update(false, true, dt), "a released charge is not a tap");

  // A reset while held (controls frozen) forgets the press.
  tap.Update(true, true, dt);
  tap.Reset();
  Check(!tap.Update(false, true, dt), "reset forgets the press");

  // A button held through a reset (into morph ball) waits for its release.
  tap.Reset();
  Check(!tap.Update(true, false, dt), "held into the ball does not spring");
  Check(!tap.Update(false, false, dt), "its release does nothing");
  Check(tap.Update(true, false, dt), "the next press springs");

  // The game's tap time is the boost's minimum charge: a release at it is a boost.
  tap.Update(false, true, dt);
  tap.Update(true, true, dt);
  tap.Update(true, true, 0.1f);
  Check(!tap.Update(false, true, dt, 0.1f), "a release at the tap time is not a tap");

  std::puts("input map tests passed");
  return 0;
}
