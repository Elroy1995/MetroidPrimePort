#pragma once

class CStateManager;
class CPlayerGun;
class CTransform4f;

// Linked only with MP_ENABLE_SMOKE_DRIVER. Never grabs the user's real pointer.
bool PortSmokeMouseEnabled();
bool PortSmokeScriptedInput();
void PortSmokeAreaReload(CStateManager& mgr);
void PortSmokeWorldTeleport(CStateManager& mgr);
void PortSmokeVisor(CStateManager& mgr);
void PortSmokeSave(CStateManager& mgr);
void PortSmokeScript(unsigned frame);
unsigned PortSmokeCurrentFrame();
void PortSmokeCurrentFrameSet(unsigned frame);
void PortSmokeStick(CStateManager& mgr);
void PortSmokeWalk(CStateManager& mgr);
unsigned PortSmokeMouseButtons(unsigned realButtons);
void PortSmokeMouseBeforeUpdate(CStateManager& mgr);
void PortSmokeMouseAfterUpdate(CStateManager& mgr);
void PortSmokeMouseShot(bool charged, bool secondary);
void PortSmokeMouseGunView(const CStateManager& mgr, const CPlayerGun& gun,
                          const CTransform4f& worldView);
