#ifndef _CGAMEARCHITECTURESUPPORT
#define _CGAMEARCHITECTURESUPPORT

#include "types.h"
#include "port_timing.h"

#include "GuiSys/CGuiSys.hpp"

#include "Kyoto/Audio/CAudioSys.hpp"
#include "Kyoto/Basics/COsContext.hpp"
#include "Kyoto/Basics/CStopwatch.hpp"
#include "Kyoto/TOneStatic.hpp"

#include "MetroidPrime/CArchitectureQueue.hpp"
#include "MetroidPrime/CIOWinManager.hpp"
#include "MetroidPrime/CInputGenerator.hpp"

#include "rstl/vector.hpp"

class CToken;

class CGameArchitectureSupport : public TOneStatic< CGameArchitectureSupport > {
public:
  enum EAudioLoadStatus { kALS_Loading, kALS_Loaded, kALS_Uninitialized };

  CGameArchitectureSupport(COsContext&);
  ~CGameArchitectureSupport();

  void PreloadAudio();
  bool UpdateTicks();
  void Update();
  bool LoadAudio();
  void UnloadAudio();

  inline CStopwatch& GetStopwatch1() { return x20_tickStopwatch; }
  inline CStopwatch& GetStopwatch2() { return x28_frameStopwatch; }
  inline CIOWinManager& GetIOWinManager() { return x58_ioWinMgr; }
  inline int& GetFramesDrawn() { return x78_gameFrameCount; }
  float GetTickInterpolation() const { return x7c_tickClock.Interpolation(); }
  float GetTickPeriod() const { return static_cast< float >(x7c_tickClock.Period()); }
  bool IsInfiniteLoopAlarmSet() const { return xc8_infiniteLoopAlarmSet; }
  OSAlarm& GetInfiniteLoopAlarm() { return xa0_infiniteLoopAlarm; }
  void SetInfiniteLoopAlarmSet(bool set) { xc8_infiniteLoopAlarmSet = set; }

private:
  CAudioSys x0_audioSys;
  CArchitectureQueue x4_archQueue;
  CStopwatch x20_tickStopwatch;
  CStopwatch x28_frameStopwatch;
  CInputGenerator x30_inputGenerator;
  CGuiSys x44_guiSys;
  CIOWinManager x58_ioWinMgr;
  int x78_gameFrameCount;
  PortTiming::FixedStepClock x7c_tickClock;
  EAudioLoadStatus x88_audioLoadStatus;
  rstl::vector< CToken > x8c_pendingAudioGroups;
  OSAlarm xa0_infiniteLoopAlarm;
  bool xc8_infiniteLoopAlarmSet;
};
CHECK_SIZEOF(CGameArchitectureSupport, 0xd0)

#endif // _CGAMEARCHITECTURESUPPORT
