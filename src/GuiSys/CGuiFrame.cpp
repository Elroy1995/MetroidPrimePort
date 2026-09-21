#include "GuiSys/CGuiFrame.hpp"

#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiFeeHelper.hpp"
#include "GuiSys/CGuiHeadWidget.hpp"
#include "GuiSys/CGuiLight.hpp"
#include "GuiSys/CGuiModel.hpp"
#include "GuiSys/CGuiSys.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Input/CFinalInput.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include "rstl/algorithm.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace rstl {
class CWidgetFartherFromCamera {
public:
  CWidgetFartherFromCamera() {}
  bool operator()(const CGuiWidget* a, const CGuiWidget* b) const {
    const CVector3f aPos = a->GetWorldPosition();
    const CVector3f bPos = b->GetWorldPosition();
    return aPos.GetY() > bPos.GetY();
  }
};
} // namespace rstl

CGuiFrame::CGuiFrame(uint id, CGuiSys& sys, int a, int b, int c, CSimplePool* sp)
: x0_id(id)
, x4_(0)
, x8_guiSys(sys)
, xc_headWidget(nullptr)
, x10_rootWidget(nullptr)
, x14_camera(nullptr)
, x3c_lights(rstl::vector< CGuiLight* >(8, static_cast< CGuiLight* >(nullptr),
                                       rstl::rmemory_allocator()))
, x4c_a(a)
, x50_b(b)
, x54_c(c)
, x58_24_loaded(false) {
  x10_rootWidget = rs_new CGuiWidget(CGuiWidget::CGuiWidgetParms(
      this, false, CGuiWidget::gkDummyWidgetID, CGuiWidget::gkDummyWidgetID, false, false,
      false, CColor::White(), CGuiWidget::kGMDF_Alpha, false,
      !x8_guiSys.GetIsUsedInGame()));
}

CGuiFrame::~CGuiFrame() {
  if (x10_rootWidget) {
    delete x10_rootWidget;
  }
}

CGuiFrame* CGuiFrame::CreateFrame(uint id, CGuiSys& sys, CInputStream& in, CSimplePool* sp) {
  uint version = in.Get< uint >();
  int a = in.ReadLong();
  int b = in.ReadLong();
  int c = in.ReadLong();
  CGuiFrame* frame = rs_new CGuiFrame(id, sys, a, b, c, sp);
  CGuiFeeHelper::SetCurrentLoadingFrame(frame);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  frame->LoadWidgetsInGame(in, sp, version);
#else
  frame->LoadWidgetsInGame(in, sp);
#endif
  return frame;
}

#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
int CGuiFrame::LoadWidgetsInGame(CInputStream& in, CSimplePool* sp, uint version) {
#else
int CGuiFrame::LoadWidgetsInGame(CInputStream& in, CSimplePool* sp) {
#endif
  int count = in.Get< int >();
  x2c_widgets.reserve(count);
  x18_db.Reserve(count);
  for (int i = 0; i < count; ++i) {
    FourCC type = in.ReadLong();
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
    CGuiWidget* widget = FGuiWidgetFactoryInGame(type, this, in, sp, version);
#else
    CGuiWidget* widget = CGuiSys::CreateWidgetInGame(type, in, this, sp);
#endif
    if (widget->GetWidgetTypeID() != 'CAMR' && widget->GetWidgetTypeID() != 'LITE' &&
        widget->GetWidgetTypeID() != 'BGND') {
      x2c_widgets.push_back(widget);
    }
  }
  Initialize();
  return 0;
}

void CGuiFrame::Initialize() {
  SortDrawOrder();
  CGuiHeadWidget* head = xc_headWidget;
  head->SetColor(head->GetColor());
  head->InitializeRecursive();
}

void CGuiFrame::Draw(const CGuiWidgetDrawParms& parms) const {
  CGraphics::SetCullMode(kCM_None);
  CGraphics::ResetGfxStates();
  CGraphics::SetAmbientColor(CColor::White());
  DisableLights();
  x14_camera->Draw(parms);
  CGraphics::SetTevOp(kTS_Stage0, CGraphics::kEnvModulate);
  CGraphics::SetBlendMode(kBM_Blend, kBF_SrcAlpha, kBF_InvSrcAlpha, kLO_Clear);
  // Widescreen HUD: spread compact elements rigidly, but stretch screen-spanning
  // decoration about the view axis. Restore each transform after drawing.
  const float spread = x14_camera->GetAspectSpread();
  const float spreadCenterX = x14_camera->GetAspectSpreadCenterX();
  // Perspective elements move in their own plane and turn in place to face the
  // eye, preserving their depth ordering. Orthographic elements only slide.
  const bool aboutEye = spread != 1.f && x14_camera->GetAspectSpreadAboutEye();
  const CTransform4f spreadView =
      aboutEye ? x14_camera->GetAspectSpreadView() : CTransform4f::Identity();
  const CTransform4f invView = aboutEye ? spreadView.GetInverse() : CTransform4f::Identity();
  const CTransform4f stretch =
      aboutEye ? invView * CTransform4f::Scale(spread, 1.f, 1.f) * spreadView
               : CTransform4f::Identity();
  const CGraphics::CProjectionState& projection = CGraphics::GetProjectionState();
  const float authoredQuarterWidthPerDepth =
      aboutEye ? (projection.GetRight() - projection.GetLeft()) /
                     (4.f * projection.GetNear() * spread)
               : 0.f;
  // The spread is a remap of the authored layout, so it has to be a pure
  // function of where the element was authored to sit. Measuring from a
  // per-frame reference instead (the centroid of whichever widgets happen to be
  // visible) makes the same screen position map differently in each HUD frame -
  // and the HUD is several independent frames, so elements that belong together
  // register differently. Spread about the view axis, which is the screen
  // centre, using the element's own depth, so any two elements at the same place
  // get the same answer whichever frame draws them.
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    CGuiWidget* widget = *it;
    if (!widget->GetIsVisible()) {
      continue;
    }
    if (spread == 1.f) {
      widget->Draw(parms);
      continue;
    }
    if (aboutEye && widget->GetParent() != nullptr) {
      const CTransform4f world = widget->GetWorldTransform();
      if (widget->GetWidgetTypeID() == 'MODL' && widget->GetIsFinishedLoading()) {
        const auto& token = static_cast< const CGuiModel* >(widget)->GetModel();
        const CModel* model = token ? token->GetObject() : nullptr;
        if (model) {
          // SetViewPointMatrix takes a camera-to-world transform; camera-local
          // +Y is forward (the graphics layer maps it to projection-space -Z).
          const CAABox bounds = model->GetBoundingBox().GetTransformedAABox(invView * world);
          // Require the mesh bounds to reach both outer quarters of the authored
          // viewport, even at their farthest depth. Unlike extent / translation,
          // this does not stretch small icons at the origin or under a translated
          // parent. Only a fixed-size bounding box is examined, never vertices.
          const float farDepth = bounds.GetMaxPoint().GetY();
          const float quarterWidth = farDepth * authoredQuarterWidthPerDepth;
          if (bounds.GetMinPoint().GetY() > projection.GetNear() && quarterWidth > 0.f &&
              bounds.GetMinPoint().GetX() < -quarterWidth &&
              bounds.GetMaxPoint().GetX() > quarterWidth) {
            // This intentionally widens vertical strokes too, but leaves view Y
            // and depth untouched; compact widgets retain the rigid path below.
            widget->SetO2WTransform(stretch * world);
            widget->Draw(parms);
            widget->SetO2WTransform(world);
            continue;
          }
        }
      }
      const CVector3f eyePos = spreadView * world.GetTranslation();
      const float across = eyePos.GetX();
      const float depth = eyePos.GetZ() < 0.f ? -eyePos.GetZ() : eyePos.GetZ();
      if (depth <= 1.f) {
        // At or behind the eye plane: no sensible tangent to spread.
        widget->Draw(parms);
        continue;
      }
      const float yaw = std::atan2(across, depth);
      // Spreading in tangent space maps the authored frustum exactly onto the
      // wider one, so an element at the edge lands at the edge.
      const float delta = std::atan(std::tan(yaw) * spread) - yaw;
      // Move the element across the screen inside its own plane, then turn it in
      // place so it still faces the eye. Rotating each element about the eye
      // instead swings it closer to the eye, and the widest elements swing
      // closest, which reorders HUD depths - the visor ends up drawn over the
      // elements it should sit behind. Doing this in eye space keeps the move on
      // the camera's own axes no matter which way the player faces.
      const float dx = (spread - 1.f) * across;
      const CVector3f pivot = eyePos + CVector3f(dx, 0.f, 0.f);
      const CTransform4f move = CTransform4f::Translate(CVector3f(dx, 0.f, 0.f));
      const CTransform4f turn = CTransform4f::Translate(pivot) *
                                CTransform4f::RotateY(CRelAngle(delta)) * CTransform4f::Translate(-pivot);
      widget->SetO2WTransform(invView * turn * move * spreadView * world);
      widget->Draw(parms);
      widget->SetO2WTransform(world);
    } else {
      const float x = widget->GetWorldPosition().GetX();
      const float dx = (spreadCenterX + (x - spreadCenterX) * spread) - x;
      widget->MoveInWorld(CVector3f(dx, 0.f, 0.f));
      widget->Draw(parms);
      widget->MoveInWorld(CVector3f(-dx, 0.f, 0.f));
    }
  }
  CGraphics::SetCullMode(kCM_Front);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  CGraphics::SetDepthWriteMode(true, kE_LEqual, true);
#endif
}

void CGuiFrame::Update(float dt) { xc_headWidget->Update(dt); }

void CGuiFrame::ProcessUserInput(const CFinalInput& input) {
  if (input.ControllerNumber() == 0) {
    for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
      CGuiWidget* widget = *it;
      if (widget->GetIsActive()) {
        widget->ProcessUserInput(input);
      }
    }
  }
}

void CGuiFrame::Touch() const {
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    (*it)->Touch();
  }
}

bool CGuiFrame::GetIsFinishedLoading() const {
  if (x58_24_loaded) {
    return true;
  }
  x58_24_loaded = true;
  for (AUTO(it, x2c_widgets.begin()); it != x2c_widgets.end(); ++it) {
    // Models stream in asynchronously and CGuiModel::Draw already skips itself
    // until ready. Blocking the whole frame on them meant a single slow/stuck
    // model texture kept the entire HUD (including the energy/missile bars)
    // hidden, e.g. after a morph-ball round trip.
    if ((*it)->GetWidgetTypeID() == 'MODL') {
      continue;
    }
    if (!(*it)->GetIsFinishedLoading()) {
      x58_24_loaded = false;
      if (std::getenv("MP_LOG_HUD") != nullptr) {
        static int sLogCount = 0;
        if (sLogCount < 80) {
          ++sLogCount;
          const FourCC type = (*it)->GetWidgetTypeID();
          std::fprintf(stderr, "[hud] frame blocked by %c%c%c%c id=%u\n",
                       static_cast< char >(type >> 24), static_cast< char >(type >> 16),
                       static_cast< char >(type >> 8), static_cast< char >(type),
                       (*it)->GetWidgetID());
        }
      }
      return false;
    }
  }
  return true;
}

void CGuiFrame::AddLight(CGuiLight* light) { x3c_lights[light->xd8_lightId] = light; }

void CGuiFrame::RemoveLight(CGuiLight* light) {
  if (x3c_lights[light->xd8_lightId] == light) {
    x3c_lights[light->xd8_lightId] = nullptr;
  }
}

void CGuiFrame::DisableLights() const { CGraphics::DisableAllLights(); }

void CGuiFrame::EnableLights(uint mask) const {
  CGraphics::DisableAllLights();
  CColor ambient = CColor::Black();
  int enabledLights = 0;
  for (int i = 0; i < x3c_lights.size(); ++i) {
    if (mask & (1 << i)) {
      CGuiLight* light = x3c_lights[i];
      if (light && light->GetIsVisible()) {
        const CColor& color = light->GetModifiedColor();
        if (color.GetRedu8() != 0 || color.GetGreenu8() != 0 || color.GetBlueu8() != 0) {
          CGraphics::LoadLight(static_cast< ERglLight >(i), light->BuildLight());
          CGraphics::EnableLight(static_cast< ERglLight >(i));
        }
        ambient = CColor::Add(ambient, CColor(light->xdc_ambColor));
        ++enabledLights;
      }
    }
  }
  if (enabledLights == 0) {
    CGraphics::SetAmbientColor(CColor::White());
  } else {
    CGraphics::SetAmbientColor(ambient);
  }
}

void CGuiFrame::SortDrawOrder() {
  rstl::sort(x2c_widgets.begin(), x2c_widgets.end(), rstl::CWidgetFartherFromCamera());
}

void CGuiFrame::RemoveWidgetFromDrawList(CGuiWidget* widget) {
  AUTO(it, x2c_widgets.begin());
  AUTO(end, x2c_widgets.end());
  for (; it != end; ++it) {
    if (*it == widget) {
      x2c_widgets.erase(it);
      break;
    }
  }
}

CGuiWidget* CGuiFrame::FindWidget(const rstl::string& name) const {
  short id = x18_db.FindWidgetID(name);
  if (id != CGuiWidget::InvalidWidgetId()) {
    return FindWidget(id);
  }
  return nullptr;
}

CGuiWidget* CGuiFrame::FindWidget(short id) const { return x10_rootWidget->FindWidget(id); }

void CGuiFrame::SetHeadWidget(CGuiHeadWidget* widget) { xc_headWidget = widget; }

void CGuiFrame::SetFrameCamera(CGuiCamera* camera) { x14_camera = camera; }

CGuiWidget* CGuiFrame::FindWidget(const char* name) const { return FindWidget(rstl::string_l(name)); }

CGuiLight* CGuiFrame::GetFrameLight(int idx) { return x3c_lights[idx]; }
