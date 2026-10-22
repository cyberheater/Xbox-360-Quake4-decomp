# 120 Hz game logic

Quake 4 on the Xbox 360 runs its game logic at 60 tics per second and renders one frame per tic.
The engine was written to support other rates: the game asks the engine for its rate
(`idCommon::GetUserCmdHz` / `GetUserCmdMSec`; multiplayer used 30 Hz) and derives the game's
time step, the async tic timer and controller turning from it. `src/q4_tick.cpp` overrides these
two functions so single player runs at **120 tics per second, 8 ms of game time per tic**.

## Turning it on

* Default (`q4_tickrate = 0`, auto): `q4hle.exe` reads the primary display's refresh rate at
  startup and runs at 120 Hz on displays of 120 Hz or more (120/144/165/240), otherwise 60 Hz.
  stderr shows the choice (`q4: display refresh N Hz -> game rate M Hz`). `q4.exe` treats auto as
  60. The timing fixes cover exactly 60 and 120 Hz, so other refresh rates map to one of them.
* `q4_tickrate = 60` or `120` in `q4.toml` (or `Q4_TICRATE=60/120` in the environment) forces a rate.
* Only `q4hle.exe` (native renderer) shows more than 60 fps: its null GPU emulates a vblank at the
  game rate. `q4.exe` (Xenia GPU) keeps a 60 Hz vblank and still shows 60 fps.

Use a 120 Hz (or faster) display; the native renderer presents without vsync by default.

## Why timing stays intact

* The async tic timer fires every 8 ms, so the game runs 125 tics per real second x 8 ms = 1000 ms
  of game time per real second, exactly as at 60 Hz (62.5 tics x 16 ms). Everything driven by game
  time or real time is unchanged: animations, cinematics and camera paths, lip sync, sound and
  dialogue, timers, `sys.wait()`, projectiles and physics (all scale with `gameLocal.msec`).
* Script frame waits (`sys.waitFrame()`, `$entity.waitFrame()`) still last 16 ms (two tics), and
  the script events `getFrameTime` / `getTicsPerSecond` still report 60 Hz, so map scripts that
  count frames or step something per frame (boss shields, fades, turning) behave as shipped.
* Code that does something once per game frame with a constant step, factor or random roll would
  otherwise run twice as fast. These places were found by auditing the SDK source and the 360
  executable and are corrected with mid-function hooks (`q4_tick_hooks.toml` +
  `src/q4_tick_hooks_*.cpp`, 69 hooks, all no-ops at 60 Hz and in multiplayer):
  * AI: complex-think round-robin (reaction time, pain windows, CPU), lead-aim smoothing, head
    focus and look blending, flyer banking, Convoy hover AI, Strogg hover bomb climb and death
    spiral, medic heal ticks, Light Tank power-up regeneration, death push impulses.
  * Player and physics: crouch eye-height smoothing, pushing objects, standing on monsters,
    influence view clamp, air timer, weapon turning lag, dynamic protection recovery, rigid-body
    contact friction, ragdoll (AF) friction and velocity caps, force fields.
  * Vehicles: Walker strafe and stomp damage, hull-to-turret alignment, hover-pad damping, thrusters,
    vehicle weapon refire.
  * Weapons: refire intervals keep their 60 Hz 16 ms granularity (machine gun 112 ms, lightning
    64 ms, ...), blaster charge, dark matter damage ticks.
  * Engine: catch-up clamps (160 ms), camera animations authored at 60 fps (frame timing as at
    60 Hz, interpolated in between), playback blend-in, controller stick smoothing.
* Remaining differences (by design): the GUI system still updates at most every 16 ms (menus
  look identical), and anything rendered moves twice as smoothly.

Audit notes: `docs/tickrate_audit/` (game code audit, script audit, per-group hook notes, review).

## Performance

At 120 Hz every game tic costs about as much CPU as a 60 Hz frame did. On the test PC most of the
game holds 120 fps. The game's own per-tic work (recompiled PowerPC code on one thread) takes
6-8 ms in the heaviest outdoor fights (Mars landing). The native renderer now runs D3D11 on its own
thread and only costs the game thread about 0.8-1.1 ms per frame (see `game-thread capture` in the
`renderer cpu` lines of q4hle.log), so those fights run at 8.3-8.9 ms frames (about 112-120 fps)
instead of 95-120 fps before the render thread.

## CPU profile at 120 Hz (Mars landing, Ryzen 7 5700X)

The game's main thread does everything in sequence: game logic (~25% of a heavy frame: AI,
physics, animation), the renderer front end (~40%: visibility, interactions, shadow volumes,
skinning, particles) and the back end that issues the D3D calls (~25-28%, about half of it the
native renderer's capture). The render thread is ~90% idle.

Fixes (`src/q4_perf.cpp`, on by default, `Q4_PERF_NATIVE=0` to compare):
* The session's tic wait polled `Sys_Sleep(1)`; host 1 ms sleeps last 1.5-2 ms, so frames that
  were on time missed the 8.3 ms vblank. The wait now watches `com_ticNumber` directly.
* Native `sin`, `cos`, `memcpy` instead of the recompiled CRT versions.
* Native renderer capture: no zero-filling of capture buffers, pooled constant blocks.
Together: 25% fewer frames over 9.5 ms on the same replay. Building the recompiled code with
AVX2 (`-DQ4_AVX2=ON`) measured no gain and is off by default.

The game's renderer back end (the code that walks the frame's command list and makes the D3D
calls) now runs on its own thread, one frame behind, overlapping the next game tic and front end
(docs/hle.md, "Back-end thread"): on the same replay, frames over 9.5 ms in gameplay dropped from
about 620 to about 125 and the worst second from 92.7 to 110 fps.

Inherent: the game runs 125 tics per second (8 ms) while a 120 Hz display shows 120 frames, so
about 5 frames per second run two game tics. In heavy scenes those frames can miss a vblank.
