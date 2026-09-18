// Temporary SDK stubs for functions the Aurora compatibility layer does not
// implement. These let the Metroid Prime port link; several will need real
// implementations for correct audio/VI/OS behaviour. Signatures are taken from
// the same headers the game compiled against (Aurora's dolphin/*).

#include <cmath>
#include <cstdio>

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/dtk.h>
#include <dolphin/gx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

// CElementGen.cpp declares this SDK entry point directly.
extern "C" float frsqrte(float x);

// --- AI (audio DMA) ---------------------------------------------------------
extern "C" u32 AIGetDMAStartAddr(void) {
    return 0;
}
extern "C" void AIInit(u8* stack) {
    (void)stack;
}
extern "C" void AIInitDMA(uintptr_t start_addr, u32 length) {
    (void)start_addr;
    (void)length;
}
extern "C" AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    (void)callback;
    return nullptr;
}
extern "C" void AISetStreamPlayState(u32 state) {
    (void)state;
}

// --- AR (ARAM) --------------------------------------------------------------
extern "C" u32 ARGetDMAStatus(void) {
    return 0;
}

// --- DTK (streamed audio tracks) --------------------------------------------
extern "C" void DTKInit(void) {}
extern "C" u32 DTKGetState(void) {
    return 0;
}
extern "C" int DTKSetState(u32 state) {
    (void)state;
    return 0;
}
extern "C" int DTKNextTrack(void) {
    return 0;
}
extern "C" int DTKFlushTracks(DTKFlushCallback callback) {
    (void)callback;
    return 0;
}
extern "C" u32 DTKQueueTrack(char* fileName, DTKTrack* track, u32 eventMask, DTKCallback callback) {
    (void)fileName;
    (void)track;
    (void)eventMask;
    (void)callback;
    return 0;
}
extern "C" void DTKSetRepeatMode(u32 repeat) {
    (void)repeat;
}
extern "C" void DTKSetSampleRate(u32 samplerate) {
    (void)samplerate;
}
extern "C" void DTKSetVolume(u8 left, u8 right) {
    (void)left;
    (void)right;
}

// --- GX gaps ----------------------------------------------------------------
extern "C" void GXAbortFrame(void) {}
extern "C" void GXInitFifoLimits(GXFifoObj* fifo, u32 hiWaterMark, u32 loWaterMark) {
    (void)fifo;
    (void)hiWaterMark;
    (void)loWaterMark;
}
extern "C" void GXInvalidateTexRegion(const GXTexRegion* region) {
    (void)region;
}
extern "C" void GXSetMisc(GXMiscToken token, u32 val) {
    (void)token;
    (void)val;
}

// --- OS gaps ----------------------------------------------------------------
extern "C" void OSCancelAlarm(OSAlarm* alarm) {
    (void)alarm;
}
extern "C" BOOL OSDisableInterrupts(void) {
    return TRUE;
}
extern "C" BOOL OSEnableInterrupts(void) {
    return TRUE;
}
extern "C" u32 OSGetConsoleType(void) {
    return 0;
}
extern "C" OSThread* OSGetCurrentThread(void) {
    // Non-null dummy so callers that register/unregister do not dereference
    // null. Give it a plausible guest stack range inside MEM1.
    static OSThread s_dummyThread{};
    static bool init = false;
    if (!init) {
        init = true;
        s_dummyThread.stackBase = reinterpret_cast< u8* >(0x81800000u);
        s_dummyThread.stackEnd = reinterpret_cast< u8* >(0x81700000u);
        s_dummyThread.state = OS_THREAD_STATE_RUNNING;
    }
    return &s_dummyThread;
}
extern "C" u32 OSGetProgressiveMode(void) {
    return 0;
}
extern "C" BOOL OSGetResetButtonState(void) {
    return FALSE;
}
extern "C" void OSGetSavedRegion(void** start, void** end) {
    if (start != nullptr) {
        *start = nullptr;
    }
    if (end != nullptr) {
        *end = nullptr;
    }
}
extern "C" u32 OSGetSoundMode(void) {
    return 0;
}
extern "C" BOOL OSLink(OSModuleInfo* newModule, void* bss) {
    (void)newModule;
    (void)bss;
    return TRUE;
}
extern "C" void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control) {
    (void)chan;
    (void)addr;
    (void)nBytes;
    (void)control;
}
extern "C" void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)reset;
    (void)resetCode;
    (void)forceMenu;
}
extern "C" BOOL OSRestoreInterrupts(BOOL level) {
    (void)level;
    return TRUE;
}
extern "C" u32 OSSaveContext(OSContext* context) {
    (void)context;
    return 0;
}
extern "C" OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler) {
    (void)error;
    (void)handler;
    return nullptr;
}
extern "C" void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    (void)alarm;
    (void)start;
    (void)period;
    (void)handler;
}
extern "C" void OSSetProgressiveMode(u32 on) {
    (void)on;
}
extern "C" void OSSetSaveRegion(void* start, void* end) {
    (void)start;
    (void)end;
}
extern "C" void OSSetSoundMode(u32 mode) {
    (void)mode;
}
extern "C" BOOL OSUnlink(OSModuleInfo* oldModule) {
    (void)oldModule;
    return TRUE;
}
extern "C" void OSYieldThread(void) {}

// --- VI gaps ----------------------------------------------------------------
extern "C" u32 VIGetDTVStatus(void) {
    return 0;
}
extern "C" u32 VIGetNextField(void) {
    return 0;
}
extern "C" void VISetBlack(BOOL black) {
    (void)black;
}
extern "C" void VISetNextFrameBuffer(void* fb) {
    (void)fb;
}
extern "C" VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb) {
    (void)cb;
    return nullptr;
}
extern "C" VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb) {
    (void)cb;
    return nullptr;
}
extern "C" void VIWaitForRetrace(void) {}

// --- Aurora-declared but not linked -----------------------------------------
// Aurora lists these in headers but leaves the implementations as TODOs (and
// OSFatal's definition does not link), so provide them here.
extern "C" void GXInitTexCacheRegion(GXTexRegion* region, GXBool is_32b_mipmap, u32 tmem_even,
                                     GXTexCacheSize size_even, u32 tmem_odd,
                                     GXTexCacheSize size_odd) {
    (void)region;
    (void)is_32b_mipmap;
    (void)tmem_even;
    (void)size_even;
    (void)tmem_odd;
    (void)size_odd;
}
extern "C" GXTexRegionCallback GXSetTexRegionCallback(GXTexRegionCallback callback) {
    (void)callback;
    return nullptr;
}
extern "C" void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    if (msg != nullptr) {
        std::fprintf(stderr, "OSFatal: %s\n", msg);
    }
}

// --- PowerPC math -----------------------------------------------------------
extern "C" float frsqrte(float x) {
    return 1.0f / std::sqrt(x);
}
