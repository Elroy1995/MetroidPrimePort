#pragma once

// The decompiled code targets the original Dolphin SDK headers. Aurora's
// dolphin/* mirrors the SDK API but omits a handful of Retro Studios
// convenience macros that the game code relies on. This header is force-included
// ahead of every game translation unit (see CMakeLists.txt) to restore them.

#ifndef AUTO
#if defined(__cplusplus) && __cplusplus >= 201103L
#define AUTO(name, val) auto name = val
#define AUTO_REF(name, val) auto& name = val
#define AUTO_CONST_REF(name, val) const auto& name = val
#else
#define AUTO(name, val) __typeof__(val) name = val
#define AUTO_REF(name, val) __typeof__(val)& name = val
#define AUTO_CONST_REF(name, val) const __typeof__(val)& name = val
#endif
#endif
