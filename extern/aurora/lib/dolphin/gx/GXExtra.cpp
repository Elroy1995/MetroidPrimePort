#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"

extern "C" {
void GXDestroyTexObj(GXTexObj* obj_) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  if (obj->texObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TEXOBJ);
    GX_WRITE_U32(obj->texObjId);
  }
  obj->texObjId = 0;
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  if (obj->tlutObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TLUT);
    GX_WRITE_U32(obj->tlutObjId);
  }
  obj->tlutObjId = 0;
}

void GXDestroyCopyTex(void* dest) {
  if (dest != nullptr) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_COPY_TEX);
    GX_WRITE_U64(reinterpret_cast<u64>(dest));
  }
}

void GXSetArrayBaseIndex(GXAttr attr, u32 base) {
  if (attr == GX_VA_NBT) {
    attr = GX_VA_NRM;
  }
  const u32 cpIdx = attr - GX_VA_POS;
  assert((cpIdx & ~0xF) == 0);
  GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAY_BASE_INDEX);
  GX_WRITE_U8(static_cast<u8>(cpIdx));
  GX_WRITE_U32(base);
}

void GXSetPBR(GXBool enable) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR);
  GX_WRITE_U8(enable ? 1 : 0);
}

void GXCopyProbeFace(u32 face) {
  GX_WRITE_AURORA(GX_AURORA_COPY_PROBE_FACE);
  GX_WRITE_U8(static_cast<u8>(face));
  aurora::gx::fifo::publish();
}

void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_PROBE);
  for (int col = 0; col < 3; ++col) {
    GX_WRITE_F32(viewToProbe[0][col]);
    GX_WRITE_F32(viewToProbe[1][col]);
    GX_WRITE_F32(viewToProbe[2][col]);
    GX_WRITE_F32(col == 0 ? weight : 0.f);
  }
}
}
