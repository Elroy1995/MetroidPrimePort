# Frame interpolation: scope

Status: scope only, nothing here is implemented beyond "What exists".

Goal: smooth motion above 60 FPS while the game logic stays at its console rate
(60 Hz fixed step). Rendered frames between two ticks draw the world at a blend
of the last two simulation states, and first-person look input is applied every
rendered frame. The alternative, running the simulation itself faster (`sim_rate` /
`sim_adaptive`), stays an experimental option; see "Why not `sim_rate`" below.

## What exists

- **Clock.** `PortTiming::FixedStepClock` (`platform/include/port_timing.h`)
  keeps the leftover time; `Interpolation()` = leftover / period, exposed as
  `CGameArchitectureSupport::GetTickInterpolation()`.
- **Camera.** `CCameraManager::Update` records the camera transform before and
  after each tick (`sPreviousCameraTransform` / `sCurrentCameraTransform`,
  `CCameraManager.cpp:44-58, 330-356`). A camera switch, a jump of more than 4
  units, or a turn of more than 45° (outside free mouse look) resets the
  previous snapshot, so cuts don't smear. `main.cpp` (~992) sets the blend
  factor around `IOWinManager().Draw()`, only when the frame limiter is off;
  `GetCurrentCameraTransform` then returns the lerped/slerped view.
- **Mouse aim.** Under free mouse look the view keeps this tick's orientation
  (only translation is blended), so the reticle never trails the shot
  direction. The arm cannon, arm and muzzle effects render against the matching
  simulation camera (`NATIVE_PORT.md`, "In uncapped presentation").
- **Render-time animation.** `CGraphics::TickRenderTimings` advances draw-time
  timers (texture scroll, UV animation) by whole ticks, not by frames.

So at 144 Hz today the camera glides but every actor, animated pose, particle
and projectile still steps at 60 Hz: moving objects visibly judder against a
smooth camera, and mouse look updates at 60 Hz.

## What is needed

### 1. Actor transforms

`CActor::RenderInternal` (`CActor.cpp:358-394`) draws with `x34_transform`.
Add a previous-tick transform per actor and a render transform:

- At the start of each tick (`CStateManager::Update`, before thinking), copy
  `x34_transform` into a port-only `prevTransform` for every actor.
- A port helper `RenderTransform()` returns the blend (lerp translation, slerp
  rotation, lerp scale) when a blend factor is set, else `x34_transform`.
- Reset (prev = current) on `SetTransform`/`SetTranslation` calls that are
  teleports: spawn, `CScriptActorKeyframe` jumps, warps, save-state loads, room
  loads, player respawn. Simplest rule: any per-tick move larger than a
  threshold snaps, like the camera's 4-unit check.
- Draw paths that read `x34_transform` or `GetTransform()` directly: about 100
  files override `Render`/`AddToRenderer`, and 36 override `PreRender`. Each
  needs checking; most go through `CModelData::Render(…, xf, …)` with the
  actor's transform, so swapping the argument in `CActor` and in the main
  overrides (player, morph ball, enemies with bone tracking) covers most of the
  screen. Bounds and culling can stay on the simulation transform.

### 2. Skinned poses

`CStateManager::PreRender` runs every rendered frame (`CMFGame::Draw`), and
`CAnimData::PreRender` builds the pose from the animation time, which only
moves on ticks. Two options:

- **Sample the tree ahead** by `t * dt` without firing events. `CAnimData::Advance`
  mutates the tree in place (`DoAdvance`, transitions, event dispatch), so this
  needs a side-effect-free sampling path or a cheap copy of the tree state;
  `AdvanceParticles` and events stay on the tick. Closest to correct, and the
  main unknown in this plan.
- **Blend two built poses** (keep last tick's bone matrices, slerp per bone).
  Doubles pose memory and is wrong for blends that change tree shape between
  ticks.

Bone tracking, IK chains and rag dolls (`CSpacePirate`, `CFlyingPirate`,
`CFlaahgra`, ...) compute in `PreRender` from `GetTransform()`; they must read
the render transform or they will pull limbs back to the tick position.

### 3. Particles, projectiles, effects

- `CElementGen` particles store only the current position. Needs a previous
  position per particle (memory: `CParticle` grows by 12 bytes) or drawing with
  `pos + vel * t * dt` (cheap, wrong for accelerated or orbiting particles but
  hard to see at these rates).
- Swooshes (`CParticleSwoosh`), electric (`CParticleElectric`), beams and
  projectiles (`CEnergyProjectile`, `CWaveBuster`, `CPlasmaProjectile`) have
  their own geometry; each is a separate small job.
- Effects attached to actors (locators via `CActorModelParticles`) follow once
  the owning transform is blended.

### 4. First-person view and look input

- Apply mouse and gyro deltas to the view every rendered frame: accumulate
  yaw/pitch in `PortDebug` between ticks and add the unconsumed part to the
  presented camera orientation. The tick still consumes the whole delta, so the
  shot direction is unchanged; the reticle and the render agree because both
  use the presented orientation.
- The arm cannon must use the same presented orientation, or it will swim
  against the view. Today it renders against the simulation camera; that
  becomes the presented camera plus the gun's own tick-rate animation.
- Stick look stays per-tick (it is integrated with acceleration curves in the
  game code); only its result is blended like any camera.

### 5. HUD and 2D

The combat HUD, scan visor and map mostly follow the camera or are static; the
lock-on reticle and damage indicators track world positions and must project
through the blended transforms. GUI frames (`CGuiFrame`) animate by tick and
can stay at 60 Hz for a first pass.

### 6. Resets and edge cases

Snap everything (prev = current) on: room/world load, save-state load, warp,
cinematic start/end, camera cut, player respawn, pause and unpause, the
`MP_TURBO` lockstep, and any tick where more than one step ran after a stall
(blend only the last one). Scan visor and X-ray/thermal passes copy the frame,
so they work unchanged.

## Phased plan

1. **Look input per frame** (section 4). Smallest change with the largest felt
   gain for mouse and gyro users; no per-actor state. About 1-2 days.
2. **Actor transforms** in `CActor` plus the player, morph ball and door
   paths. Turns most of the judder smooth. About 3-5 days including the audit
   of `Render` overrides and teleport resets.
3. **Render-side animation time** for skinned models and the bone-tracking/IK
   users. About 1 week; the risk is event and particle side effects of
   sampling the tree between ticks.
4. **Particles and projectiles.** About 1 week, per-type.
5. **Sweep:** ASan tour (`MP_RANDO_SWEEP`) with interpolation forced on (a
   fake fractional `t` under `MP_TURBO`), captures at t = 0/0.5/1 compared
   against tick frames to find paths that were missed.

Each phase is useful on its own and behind one setting (`frame_interpolation`,
off by default until phase 3). With the frame limiter on (the default 60 FPS
cap) nothing changes, as today.

## Why not `sim_rate`

Raising the tick rate makes every system genuinely smooth, but the game was
tuned for 60 Hz: `docs/HIGH_FPS_AUDIT.md` lists per-frame counters, fixed-step
physics constants, timers that expire when `dt` exceeds their length (the
Sunchamber dish bug) and AI that behaves differently at other rates. Each needs
fixing and testing room by room, and any miss changes gameplay. Interpolation
never changes the simulation, so its failures are visual only. `sim_rate` and
`sim_adaptive` stay in F1 Performance as experiments.
