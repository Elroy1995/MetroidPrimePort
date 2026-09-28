#include "MetroidPrime/ScriptObjects/CScriptTimer.hpp"

#include "MetroidPrime/CStateManager.hpp"

CScriptTimer::CScriptTimer(const TUniqueId uid, const rstl::string& name, const CEntityInfo& info,
                           const float startTime, const float maxRandDelay, const bool loop,
                           const bool autoStart, const bool active)
: CEntity(uid, info, active, name)
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
, x34_startFrame(0)
#endif
, x34_time(startTime)
, x38_startTime(startTime)
, x3c_maxRandDelay(maxRandDelay)
, x40_loop(loop)
, x41_autoStart(autoStart)
, x42_isTiming(autoStart)
#ifdef TARGET_PC
, mSkipNextTick(false)
#endif
{
}

CScriptTimer::~CScriptTimer() {}

void CScriptTimer::Reset(CStateManager& mgr) {
  const float rDt = mgr.Random()->Float();
  x34_time = (x3c_maxRandDelay * rDt) + x38_startTime;
}

void CScriptTimer::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId,
                                   CStateManager& stateMgr) {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  if (GetActive()) {
    switch (msg) {
    case kSM_Start:
      StartTiming(true);
      x34_startFrame = stateMgr.GetInputFrameIdx();
      break;

    case kSM_Stop:
      StartTiming(false);
      break;

    case kSM_Reset:
      Reset(stateMgr);
      if (x41_autoStart) {
        StartTiming(true);
        x34_startFrame = stateMgr.GetInputFrameIdx();
      }
      break;

    case kSM_StopAndReset:
      Reset(stateMgr);
      StartTiming(false);
      break;

    case kSM_ResetAndStart:
      Reset(stateMgr);
      StartTiming(true);
      x34_startFrame = stateMgr.GetInputFrameIdx();
      break;
    }
  }
#else
  switch (msg) {
  case kSM_Start:
    if (GetActive()) {
      StartTiming(true);
#ifdef TARGET_PC
      mSkipNextTick = true;
#endif
    }
    break;

  case kSM_Stop:
    if (GetActive()) {
      StartTiming(false);
    }
    break;

  case kSM_Reset:
    if (GetActive()) {
      Reset(stateMgr);
      if (x41_autoStart) {
        StartTiming(true);
#ifdef TARGET_PC
        mSkipNextTick = true;
#endif
      }
    }
    break;

  case kSM_StopAndReset:
    if (GetActive()) {
      Reset(stateMgr);
      StartTiming(false);
    }
    break;

  case kSM_ResetAndStart:
    if (GetActive()) {
      Reset(stateMgr);
      StartTiming(true);
#ifdef TARGET_PC
      mSkipNextTick = true;
#endif
    }
    break;
  }
#endif
  CEntity::AcceptScriptMsg(msg, objId, stateMgr);
}

void CScriptTimer::ApplyTime(float dt, CStateManager& mgr) {
  if (x34_time > 0.f && GetActive()) {
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
    if (x34_startFrame == mgr.GetInputFrameIdx()) {
      return;
    }
#endif
#ifdef TARGET_PC
    // Retail NTSC relies on dt = 1/60: a short timer (Sunchamber's 0.02 s mirror
    // timer) that some object re-arms every tick never expires. With a bigger
    // step (cutscene fast-forward, adaptive sim rate, sim rates under 50) it
    // expired every tick and spammed its sound. Like PAL's x34_startFrame check,
    // skip the first tick after a message starts the timer, whatever the order
    // the objects think in.
    if (mSkipNextTick) {
      mSkipNextTick = false;
      return;
    }
#endif
    x34_time -= dt;
    if (x34_time <= 0.f) {
      SendScriptMsgs(kSS_Zero, mgr, kSM_None);

      x42_isTiming = false;
      if (!x40_loop) {
        return;
      }

      Reset(mgr);
      if (!x41_autoStart) {
        return;
      }

      x42_isTiming = true;
    }
  }
}

void CScriptTimer::Think(float dt, CStateManager& mgr) {
  if (GetActive()) {
    bool should = false;
    if (IsTiming() && GetActive()) {
      should = true;
    }
    if (should) {
      ApplyTime(dt, mgr);
    }
  }
}

ENTITY_ACCEPT_IMPL(CScriptTimer)
