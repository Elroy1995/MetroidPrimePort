#ifndef _CGUICAMERA
#define _CGUICAMERA

#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"

class CGuiCamera : public CGuiWidget {
public:
  enum EProjection {
    kProjection_Perspective,
    kProjection_Orthographic,
  };

  union UCameraParms {
    struct {
      float fov;
      float aspect;
      float znear;
      float zfar;
    } perspective;
    struct {
      float left;
      float right;
      float top;
      float bottom;
      float znear;
      float zfar;
    } orthographic;
  };

  static CGuiWidget* Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp);
  CGuiCamera(const CGuiWidgetParms& parms, float fov, float aspect, float znear, float zfar);
  CGuiCamera(const CGuiWidgetParms& parms, float left, float right, float top, float bottom,
             float znear, float zfar);

  void Draw(const CGuiWidgetDrawParms& parms) const override;
  UCameraParms GetParms() const { return mCameraParms; }
  void SetParms(UCameraParms parms) { mCameraParms = parms; }
  // GUI cameras opt in so their projection tracks the widescreen render aspect
  // instead of stretching. Set for the in-game HUD, the pause/map screens and
  // the front end; anything left unset keeps its authored aspect.
  void SetAspectMatch(bool match) { xb9_aspectMatch = match; }
  // Widescreen HUD spread for this frame's widgets: 1.0 when inactive.
  float GetAspectSpread() const { return mSpread; }
  float GetAspectSpreadCenterX() const { return mSpreadCenterX; }
  // Under a perspective projection a widened aspect pulls off-axis elements
  // toward the centre, and simply translating them back leaves them turned away
  // from the view axis, which shears their shapes. There the spread rotates
  // each element rigidly about the eye instead, which leaves what the viewer
  // sees of it unchanged. Parallel projections do not need that.
  bool GetAspectSpreadAboutEye() const { return mSpreadAboutEye; }
  const CTransform4f& GetAspectSpreadView() const { return mSpreadView; }

  FourCC GetWidgetTypeID() const override { return 'CAMR'; }

  CVector3f ConvertToScreenSpace(const CVector3f& point) const;

public:
  EProjection xb8_projection;
  bool xb9_aspectMatch = false;
  UCameraParms mCameraParms;
  // Port: horizontal spread applied to this frame's top-level widgets so the
  // HUD reaches the wide viewport edges without distorting element shapes.
  // Computed in the const Draw() and read immediately by CGuiFrame::Draw.
  mutable float mSpread = 1.f;
  mutable float mSpreadCenterX = 0.f;
  mutable bool mSpreadAboutEye = false;
  // The view transform the last Draw used, so the frame can rotate a widget
  // about the eye rather than shunting it sideways.
  mutable CTransform4f mSpreadView = CTransform4f::Identity();
};

#endif // _CGUICAMERA
