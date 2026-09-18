// PC implementation of the GameCube audio-interface (AI) DMA path.
//
// MusyX has its own SDL stream in the vendored runtime, but streamed audio
// (front-end/in-game music via CStaticAudioPlayer, movie audio) is produced by
// filling an AI DMA buffer from the guest. On PC there is no AI hardware, so a
// service thread drives the registered DMA callback at the buffer rate and feeds
// the submitted buffer to an SDL audio stream.

#include <atomic>
#include <cstdint>
#include <mutex>

#include <SDL3/SDL.h>

#include <dolphin/ai.h>

namespace {
constexpr uint32_t kSampleRate = 32000;
// 16-bit stereo frames: 4 bytes per frame.
constexpr uint32_t kBytesPerFrame = 4;

std::mutex sMutex;
AIDCallback sCallback = nullptr;
uintptr_t sBuffer = 0;
uint32_t sLength = 0;
SDL_AudioStream* sStream = nullptr;
SDL_Thread* sThread = nullptr;
std::atomic<bool> sRunning{false};

int AiThread(void*) {
  uint64_t next = SDL_GetTicksNS();
  while (sRunning.load(std::memory_order_relaxed)) {
    AIDCallback callback = nullptr;
    {
      std::lock_guard< std::mutex > lock(sMutex);
      callback = sCallback;
    }
    if (callback != nullptr) {
      callback();
    }

    uintptr_t buffer = 0;
    uint32_t length = 0;
    {
      std::lock_guard< std::mutex > lock(sMutex);
      buffer = sBuffer;
      length = sLength;
    }
    if (sStream != nullptr && buffer != 0 && length != 0) {
      SDL_PutAudioStreamData(sStream, reinterpret_cast< const void* >(buffer), length);
    }

    const uint64_t duration =
        length != 0 ? static_cast< uint64_t >(length) * 1000000000ull /
                          (static_cast< uint64_t >(kBytesPerFrame) * kSampleRate)
                    : 5000000ull;
    next += duration;
    const uint64_t now = SDL_GetTicksNS();
    if (next < now) {
      next = now;
    }
    SDL_DelayPrecise(next - now);
  }
  return 0;
}

void EnsureStarted() {
  if (sStream != nullptr) {
    return;
  }
  if (sBuffer == 0) {
    // Silence the AI is notionally playing before the first AIInitDMA, so the
    // guest's `AIGetDMAStartAddr` always yields a readable buffer.
    static uint8_t sSilence[0x280] = {};
    std::lock_guard< std::mutex > lock(sMutex);
    sBuffer = reinterpret_cast< uintptr_t >(sSilence);
    sLength = sizeof(sSilence);
  }
  SDL_InitSubSystem(SDL_INIT_AUDIO);
  SDL_AudioSpec spec{SDL_AUDIO_S16, 2, kSampleRate};
  sStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
  if (sStream == nullptr) {
    return;
  }
  SDL_ResumeAudioStreamDevice(sStream);
  sRunning.store(true);
  sThread = SDL_CreateThread(AiThread, "AI DMA", nullptr);
}
} // namespace

// The SDK's `AIGetDMAStartAddr` returns a 32-bit address, but on the port the DMA
// buffers are 64-bit host pointers. Guest code that needs the real pointer (the
// streamed-audio mixer) uses this instead.
extern "C" uintptr_t AIPortGetDMAStartAddr(void) {
  std::lock_guard< std::mutex > lock(sMutex);
  return sBuffer;
}

extern "C" uint32_t AIGetDMAStartAddr(void) {
  std::lock_guard< std::mutex > lock(sMutex);
  return static_cast< uint32_t >(sBuffer);
}

extern "C" void AIInit(u8* stack) {
  (void)stack;
  EnsureStarted();
}

extern "C" void AIInitDMA(uintptr_t start_addr, uint32_t length) {
  EnsureStarted();
  std::lock_guard< std::mutex > lock(sMutex);
  sBuffer = start_addr;
  sLength = length;
}

extern "C" AIDCallback AIRegisterDMACallback(AIDCallback callback) {
  EnsureStarted();
  std::lock_guard< std::mutex > lock(sMutex);
  AIDCallback previous = sCallback;
  sCallback = callback;
  return previous;
}

extern "C" void AISetStreamPlayState(uint32_t state) {
  if (sStream == nullptr) {
    return;
  }
  if (state == 0) {
    SDL_PauseAudioStreamDevice(sStream);
  } else {
    SDL_ResumeAudioStreamDevice(sStream);
  }
}
