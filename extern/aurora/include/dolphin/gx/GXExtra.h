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
// Aurora extension: the emissive multiplier and backlight weight of the following PBR
// draws (see GX_AURORA_SET_PBR_MATERIAL).
void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3]);
// Aurora extension: room cubes (see GX_AURORA_CREATE_PBR_CUBE). `texels` is RGBA16Float,
// every mip of face 0 from the largest down, then face 1 and so on; it is copied.
void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length);
void GXDestroyPBRCube(u32 id);
// params: exposure, the mip a roughness of 1 samples, the mip the ambient samples along
// the normal, and the scale that takes that sample to 1 for an average direction (0 = the
// ambient is left alone). Id 0, or one never created, selects the probe.
void GXSetPBRCube(u32 id, const f32 params[4]);

void GXColor4f32(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif

#endif
