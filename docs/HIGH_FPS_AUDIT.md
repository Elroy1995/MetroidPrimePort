# High-FPS simulation audit

Goal: run the *simulation* at the display rate (120/144 Hz) instead of the fixed
60 Hz tick, with no render interpolation. This is the audit of how far the game
logic is from being frame-rate independent and what has to change.

## Current architecture

- `PortTiming::FixedStepClock` (`platform/include/port_timing.h`) fixes the step
  at `kPeriod = 1/60`. `Advance()` returns the number of whole 60 Hz ticks due
  this rendered frame; `Interpolation()` (leftover time / period) exists but is
  unused.
- `CGameArchitectureSupport::UpdateTicks` (`src/MetroidPrime/main.cpp:459`) runs
  that many ticks, each pushing `CreateTimerTick(kAMT_Game, 1/60)`, and calls
  `CInputGenerator::Update(1/60, ...)` per tick.
- The renderer draws the latest ticked state at the display rate (duplicated
  frames on high-refresh displays).
- `dt` reaches game code as `CMain` -> `CStateManager::Update(1/60)` ->
  `CActor::Think(1/60)` etc.

Conclusion: the plumbing is a single fixed step, so raising the simulation rate
means (a) making the step equal to the display frame time and (b) removing every
remaining 60 Hz assumption in the logic.

## Summary

| Subsystem | State | Effort |
| --- | --- | --- |
| Player movement, physics, collisions | dt-scaled | none |
| Animation (`CAnimData`, `Kyoto/Animation`) | dt-scaled, seconds-based timeline | none |
| Cameras, cutscenes, cinematic timing | dt-scaled | none |
| Input generator | dt parameter passed through | none |
| Script objects, layer manager, world/area | dt-scaled | none |
| GUI (`GuiSys`), HUD timers | dt-scaled | none |
| **Particle systems** (`CElementGen`, `CParticleElectric`, `CParticleSwoosh`) | time-driven, fixed 60 Hz substeps | none |
| **Decals** (`Weapons/CDecal`) | **frame-counted lifetimes** | small (needs a struct member) |
| Scattered AI/player/HUD frame counters | mixed | low-medium |
| Tick plumbing + projectile tick period | fixed 1/60 | done (experimental) |

The important result is that the engine core is **already dt-scaled**, and the
particle systems run by real time with fixed 60 Hz substeps, so they already
scale. The remaining real work is decals and the scattered counters.

Status: the tick plumbing, the projectile tick period, and an experimental
`sim_rate` setting are implemented (see "Implemented" below). Decals and the
per-frame counters are still outstanding.

## Findings

Line numbers were collected by an automated pass and spot-checked against the
source; treat them as pointers for the follow-up, not as verified targets.

### Tick plumbing (must change)

- `platform/include/port_timing.h:9` `FixedStepClock::kPeriod = 1.0 / 60.0`
- `src/MetroidPrime/main.cpp:469` `static const float tickPeriod = 1.f / 60.f;`
- `src/MetroidPrime/main.cpp:474` `x30_inputGenerator.Update(1.f / 60.f, ...)`
- `src/MetroidPrime/main.cpp:477` `CreateTimerTick(kAMT_Game, tickPeriod)`
- `src/MetroidPrime/main.cpp:820` `const double dt = 1.f / 60.f;`
- `src/MetroidPrime/main.cpp:1149` `CStreamAudioManager::Update(1.f / 60.f)`
- `src/Kyoto/Audio/CSfxManager.cpp` voice wrappers advance volume ramps per call
  (audio itself is wall-clock via the AI/MusyX backends, so this is cosmetic)

### Particles (already rate-independent)

All three particle systems accumulate **real time** and then step the simulation
in fixed `1/60` substeps until they catch up, exactly like a fixed-timestep
integrator. The frame counter they index their tables with follows that
accumulated time, so the tables resolve correctly at any tick rate.

`CElementGen`:

- `src/Kyoto/Particles/CElementGen.cpp:59` `kTickTime = 1 / 60.0` (the substep)
- `CElementGen.cpp:424` `double t = x74_curFrame * kTickTime;`
- `CElementGen.cpp:425` `dt1 = close_enough(dt, kTickTime) ? kTickTime : dt;`
- `CElementGen.cpp:436` `x78_curSeconds += dt1;` (driven by real dt)
- `CElementGen.cpp:444` `while (t < x78_curSeconds && !close_enough(t, x78_curSeconds))`
- `CElementGen.cpp:483-484` `t += kTickTime; ++x74_curFrame;` (substep, bounded by real time)

`CParticleElectric`:

- `src/Kyoto/Particles/CParticleElectric.cpp:271` `x28_currentFrame * (1.0 / 60.0)`
- `CParticleElectric.cpp:302` `while (evalTime < x30_curTime)` where `x30_curTime += dt`
- `CParticleElectric.cpp:330-331` `evalTime += 1.0 / 60.0; ++x28_currentFrame;`
- `CParticleElectric.cpp:439` `x15c_genRem += rate;` is per substep, which is fixed

`CParticleSwoosh`:

- `src/Kyoto/Particles/CParticleSwoosh.cpp:20` `kFrameTime = 1.f / 60.f` (the substep)
- `CParticleSwoosh.cpp:147,151` `advance = dt * timeScale; x30_curTime += advance;`
- `CParticleSwoosh.cpp:152` `while (x1d0_26_forceOneUpdate || evalTime < x30_curTime)`
- `CParticleSwoosh.cpp:195-196` `evalTime += kFrameTime; ++x28_curFrame;`

Caveat: `CParticleGlobals::SetEmitterTime()`/`GetValue()` and the `% PISY`
spawn cadence are expressed in substeps (1/60 s), so they quantise to 60 Hz even
when the outer tick is faster. That is a fidelity limit, not a speed error.

### Decals (frame-counted)

`src/Weapons/CDecal.cpp` ignores its `dt`:

- `CDecal.cpp:262,266,270` `x58_frameIdx >= <part>.GetLifetime()` lifetimes in frames
- `CDecal.cpp:274` `++x58_frameIdx;`
- `CDecal.cpp:51` `clr->GetValue(x58_frameIdx, color)` table lookup by frame

### Per-frame counters and cadences (scattered)

Player:

- `src/MetroidPrime/Player/CPlayer.cpp:1661-1662` `x2b0_outOfWaterTicks` (cap 2)
- `CPlayer.cpp:2756,3001` `xa2c_damageLoopSfxDelayTicks` (cap 2)
- `Player/CPlayerGun.cpp:803,1116` `x30c_rapidFireShots` +-1 per call
- `Player/CMorphBall.cpp:1434` `x1e38_wallSparkFrameCountdown -= 1`
- `CMorphBall.cpp:2718,2729` spider-ball effect `x8_curFrame` vs `x4_lifetime`
- `src/MetroidPrime/CStateManager.cpp:1276` `++x8d8_updateFrameIdx` (also the global particle seed)

Enemies:

- `Enemies/CSpacePirate.cpp:715,927,943,2712,2803`
- `Enemies/CFlyingPirate.cpp:692,1829`
- `Enemies/CMetroidBeta.cpp:995`
- `Enemies/CParasite.cpp:229`, `CSeedling.cpp:79`
- `Enemies/CWallCrawlerSwarm.cpp:737,743,979,982`
- `Weapons/CIceProjectile.cpp:200`, `Weapons/CNewFlameThrower.cpp:282,622`

Scripts / misc:

- `ScriptObjects/CFishCloud.cpp:410` `++x118_thinkCounter`
- `ScriptObjects/CScriptPickupGenerator.cpp:158` `x44_delayTimer -= 1.f`
- `Cameras/CBallCamera.cpp:1957` `x478_shortMoveCount += 1`
- `CMFGame.cpp:165` HUD message frame counter (in `CStateManager`)
- `src/MetroidPrime/CStateManager.cpp` `Update` head: `CElementGen/CParticleElectric/
  CDecal/CProjectileWeapon::SetGlobalSeed(x8d8_updateFrameIdx)` — per-tick RNG seed

### Confirmed dt-scaled (no change needed)

- `src/MetroidPrime/CPhysicsActor.cpp:112-160` `PredictMotion/Angular/Linear`
  (`dt * velocity`, `q3 * dt`, `torque * dt`)
- `src/MetroidPrime/CModelData.cpp:317` `AdvanceAnimation(float dt)` and
  `src/MetroidPrime/CAnimData.cpp:262` `AdvanceAnim` (timeline is `CCharAnimTime`
  seconds; the `1.f/60.f` at `CAnimData.cpp:843` is a one-time align integral)
- All cameras and cutscene timing (`CCinematicCamera`, `CInterpolationCamera`,
  `CPathCamera`, `CCameraManager`, `CFirstPersonCamera`, `CBallCamera` timers)
- `CWorld`/`CGameArea` `AliveUpdate(dt)`, `CScriptLayerManager`, `CWorldTransManager`
- `CInGameGuiManager`, `CSamusHud` dt methods, `GuiSys` panes/sliders
- `Kyoto/Animation` animation readers (`CAnimSourceReader`, `CAnimTree*`)

## Implemented: experimental `sim_rate`

- `PortDebug::SimRate()/SetSimRate()/SimPeriod()` (env `MP_SIM_RATE`, settings
  key `sim_rate`, slider in the F1 Performance tab, range 30..480, default 60).
- `PortTiming::FixedStepClock` gained `SetPeriod()`/`Period()`; the step is now
  runtime rather than `constexpr`.
- `CGameArchitectureSupport::UpdateTicks` sets the clock period from `SimRate()`
  and uses it for `CreateTimerTick` and `CInputGenerator::Update`.
- `CMain::RsMain` recomputes `dt` per frame and passes it to
  `CSfxManager::Update`; `CMain::UpdateStreamedAudio` uses the same period, so
  audio stays wall-clock (ticks x period == elapsed).
- `CProjectileWeapon::GetTickPeriod()` returns `PortDebug::SimPeriod()` instead
  of `1/60`, so projectile velocities and gravity scale with the tick.
- Camera presentation interpolation (`main.cpp:929`) becomes a no-op once the
  step matches the frame time, which is what "no interpolation" requires.

Verified: with `MP_SIM_RATE=120` the timing trace reports `simulation=120.0
ticks/s` with the render at 60 FPS, the mouse smoke still passes at the default
60, and all port tests pass. For real high-refresh gameplay, turn the F10 frame
cap off so both the renderer and the tick run at the display rate.

Known caveats:

- `CBloodFlower.cpp:321` and `CTargetableProjectile.cpp:59` cache
  `GetTickPeriod()` in function-local statics; a mid-session rate change leaves
  those stale.
- Decals still age per tick, so above 60 Hz they expire proportionally faster.
- Tick-indexed particle seeding (`x8d8_updateFrameIdx`) makes particle randomness
  rate-dependent.

## Conversion plan

Phase 1 - decals (remaining blocker)

- Make `CDecal` age by seconds: add a float accumulator, derive
  `x58_frameIdx = int(time * 60)` so the existing `GetValue` table reads keep
  working, and compare the (frame-unit) lifetimes against that derived index.
  This changes the `CDecal` layout, so `NESTED_CHECK_SIZEOF(CDecalManager,
  SDecal, 0x78)` has to be re-based.
- Particles need no conversion: the three systems already integrate real time in
  fixed 1/60 substeps (verified above). Only the substep granularity quantises
  effects to 60 Hz if sub-frame fidelity is ever wanted.

Phase 2 - scattered counters (list above)

- Convert each counter to seconds (`+= dt`) or to a one-shot guarded by a time
  threshold. The `x2b0_outOfWaterTicks`/`rapidFireShots` style counters are short
  debounces and can become `dt`-based. Make the particle RNG seed a function of
  accumulated time rather than `x8d8_updateFrameIdx`.

Phase 3 - tick plumbing (done)

- Implemented as the `sim_rate` setting; see "Implemented" above. Input is
  sampled once per tick, which at a display-matched rate is once per frame.

Phase 4 - verification

- Add a port smoke scenario that runs the same scripted input at 60 and 120 Hz
  and asserts that travelled distance, turn angle, jump height and animation
  phase match within tolerance (the existing `MP_SMOKE_*` driver already injects
  deterministic input).
- Re-check cutscene sync (cinematic timing is dt, but scripted POI events fire at
  particle/animation boundaries), door/elevator transitions, and save/load.

## Risks

- **Behaviour divergence.** Even with dt-scaled logic, floating-point summation
  order changes, so physics/collision outcomes and speedrun-frame tricks will
  differ from the 60 Hz original. This is a gameplay change, not just a port fix.
- **Frame-indexed data.** Particle PRT/PSLT/PISY tables and decal tables are
  authored per 60 Hz frame; running at 120 Hz means either sampling them at
  `frame = t*60` (visually fine) or authoring sub-frame interpolation.
- **`close_enough(dt, 1/60)` workarounds** in `CProjectileWeapon.cpp:148`,
  `CElementGen.cpp:425` and elsewhere assume the canonical step and must be
  revisited.
- **RNG determinism.** Seeding particles from the tick index makes them
  rate-dependent; seed from a fixed schedule or a stable frame counter instead.
