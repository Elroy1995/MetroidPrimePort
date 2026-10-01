#ifndef DOLPHIN_GXEXTRA_H
#define DOLPHIN_GXEXTRA_H
// Extra types for PC
#ifdef TARGET_PC
#include <dolphin/gx/GXStruct.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float r;
  float g;
  float b;
  float a;
} GXColorF32;

void GXDestroyTexObj(GXTexObj* obj);
void GXDestroyTlutObj(GXTlutObj* obj);
void GXDestroyCopyTex(void* dest);
// Aurora extension: offsets indexed fetches from an array by `base` elements.
// Stays in effect until changed; GXSetArray does not reset it.
void GXSetArrayBaseIndex(GXAttr attr, u32 base);
// Aurora extension: PBR shading for the following draws (see GX_AURORA_SET_PBR).
void GXSetPBR(GXBool enable);
// Aurora extension: the PBR environment probe (see GX_AURORA_COPY_PROBE_FACE and
// GX_AURORA_SET_PBR_PROBE).
void GXCopyProbeFace(u32 face);
void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight);

void GXColor4f32(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif

#endif
