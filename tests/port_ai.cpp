#include <SDL3/SDL.h>
#include <dolphin/ai.h>
#include <cstdio>
#include <cstdlib>

extern "C" void AIPortPoll();
extern "C" void AIPortSetOutputEnabled(int);
extern "C" void AIPortShutdown();
namespace {
int calls = 0;
bool unregister = false;
alignas(32) unsigned char samples[640]{};
void Callback() {
  ++calls;
  AIInitDMA(reinterpret_cast<uintptr_t>(samples), sizeof(samples));
  if (unregister)
    AIRegisterDMACallback(nullptr);
}
void Check(bool condition) { if (!condition) std::abort(); }
}
int main() {
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "SDL_AUDIO_DRIVER", "dummy", true);
  SDL_SetEnvironmentVariable(SDL_GetEnvironment(), "MP_DISABLE_AI_AUDIO", "1", true);
  AIRegisterDMACallback(Callback);
  AIPortPoll();
  const int silentCalls = calls;
  Check(silentCalls > 0);
  AIPortSetOutputEnabled(1);
  AIPortPoll();
  Check(calls - silentCalls >= 8); // enabling must create and prefill a stream
  AIPortSetOutputEnabled(0);
  SDL_Delay(30);
  unregister = true;
  AIPortPoll(); // callback unregisters itself during a catch-up batch
  AIPortShutdown();
  AIPortShutdown();
  std::puts("AI enable, callback lifetime and shutdown regressions passed");
}
