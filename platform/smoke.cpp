// Opt-in lifecycle driver for real-disc regression runs (MP_ENABLE_SMOKE_DRIVER).
#include "port_debug.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstdio>

bool PortSmokeFrame(unsigned frame) {
  static const unsigned limit = [] {
    const char* value = std::getenv("MP_SMOKE_FRAMES");
    return value != nullptr ? static_cast<unsigned>(std::strtoul(value, nullptr, 10)) : 0;
  }();
  if (limit == 0) return false;
  static SDL_Window* window = nullptr;
  if (window == nullptr) {
    int count = 0;
    SDL_Window** windows = SDL_GetWindows(&count);
    if (windows != nullptr && count > 0) window = windows[0];
    SDL_free(windows);
  }
  if (std::getenv("MP_SMOKE_LIFECYCLE") != nullptr && limit >= 240) {
    if (frame == limit / 4) {
      if (window != nullptr) SDL_HideWindow(window);
      PortDebug::SetAiAudioEnabled(false);
      PortDebug::SetMusyxAudioEnabled(false);
      std::fputs("[smoke] hide and mute\n", stderr);
    } else if (frame == limit / 4 + 30) {
      if (window != nullptr) SDL_ShowWindow(window);
      PortDebug::SetAiAudioEnabled(true);
      PortDebug::SetMusyxAudioEnabled(true);
      PortDebug::SetFrameLimitEnabled(false);
      std::fputs("[smoke] restore, unmute and uncap\n", stderr);
    } else if (frame == limit / 2) {
      PortDebug::SetFrameLimitEnabled(true);
      PortDebug::RequestReset();
      PortDebug::ResetMouseAim();
      std::fputs("[smoke] reset to menu\n", stderr);
    }
  }
  if (frame < limit) return false;
  std::fputs("[smoke] clean exit requested\n", stderr);
  return true;
}
