// q4 - 120 Hz tic fixes: player / physics / vehicle per-frame constants ("phys" group).
//
// Every hook is a no-op at 60 Hz (!q4tick::Fast()). Addresses are 360 executable instruction
// addresses; the matching [[midasm_hook]] entries are in hooks_phys.toml. SDK references are
// Quake 4 SDK 1.4.2 (game/...). "before" = hook runs before the instruction, "after" = after it.
#include "q4_tick.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// Per-frame fraction (remove r of something each frame) converted to one tic, for 0 <= r <= 1.
// Values outside that range are left alone (pow of a negative base would give NaN).
inline double SafeRateFrac(double r) {
  if (!(r > 0.0 && r < 1.0)) return r;
  return q4tick::RateFrac(r);
}
// Per-frame multiplicative factor f > 0 converted to one tic.
inline double SafePowFrac(double f) {
  if (!(f > 0.0)) return f;
  return q4tick::PowFrac(f);
}

}  // namespace

// =============================================================================================
// Vehicles
// =============================================================================================

// #7  rvVehicleTurret::RunPostPhysics (0x82568B80), VehicleParts.cpp:1653-1658 (g_vehicleMode != 0):
//   deg = ClampFloat(0, spawnArgs.GetFloat("alignment_delta","2.4"), RAD2DEG(acosForward)) * power;
// The hull is rotated toward the turret by up to alignment_delta degrees per frame.
//   8256950c  bl    0x828403c8          ; atof("alignment_delta") -> f1
//   82569514  frsp  f13,f1              ; f13 = alignment_delta
//   8256952c  fsubs f4,f0,f13 / 82569530 fsel f0,f4,f13,f0   ; clamp to f13
//   82569534  fmuls f0,f0,f31           ; deg = clamped * power      <- hook after
//   82569570  fmuls f31,f1,f0           ; delta.yaw = SignZero(dotRight) * deg (only use of f0)
// power = Pow(Cos(dotForward), alignment_power) is ~0.05 when nearly aligned, so there the step is
// acos * power, a per-frame proportional rate the clamp limit does not touch (review fix: the hook
// used to scale only the clamp limit f13 at 0x82569514). Scaling deg halves both regimes.
void Q4Tick_TurretAlignDelta(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(f0.f64 * q4tick::Frac()));
}

// #5  rvVehicleWalker::HandleStrafing (0x825B9148), Vehicle_Walker.cpp:162-168:
//   additionalDelta -= / += spawnArgs.GetVector("strafe_delta")   (walker.def "0 0 2.8")
// additionalDelta is added to the per-frame animation delta (VehicleAnimated.cpp:94), so the
// constant is a per-frame move distance.
//   825b9190/94/98  lfs f13,f9,f7 = strafe_delta.xyz ; 825b91a4 fsubs f12,f0,f13 <- hook before (right)
//   825b91e8/ec/f0  lfs f4,f0,f12 = strafe_delta.xyz ; 825b91fc fadds f3,f5,f4  <- hook before (left)
void Q4Tick_WalkerStrafeRight(PPCRegister& f13, PPCRegister& f9, PPCRegister& f7) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::Frac();
  f13.f64 = double(float(f13.f64 * k));
  f9.f64 = double(float(f9.f64 * k));
  f7.f64 = double(float(f7.f64 * k));
}
void Q4Tick_WalkerStrafeLeft(PPCRegister& f4, PPCRegister& f0, PPCRegister& f12) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::Frac();
  f4.f64 = double(float(f4.f64 * k));
  f0.f64 = double(float(f0.f64 * k));
  f12.f64 = double(float(f12.f64 * k));
}

// #6  rvVehicleWalker::Think (0x825B8D30), Vehicle_Walker.cpp:86-90:
//   animator.GetDelta(time - msec, time, delta);
//   if (delta.LengthSqr() > 0.1f) gameLocal.RadiusDamage(..., "def_stompDamage");
// The threshold is on a per-frame distance (an 8 ms delta is half as long, a quarter in
// LengthSqr), and the stomp damage/push is applied once per frame (twice as often per second).
//   825b8dc0  fmadds f10,f13,f13,f11    ; f10 = delta.LengthSqr()
//   825b8dbc  lfs    f0,-31084(r11)     ; 0.1
//   825b8dc4  fcmpu  cr6,f10,f0         <- hook before
//   825b8dc8  ble    0x825b8e30         ; skip RadiusDamage
// Fix: compare the LengthSqr scaled to a 16 ms delta, and only on tics that end a 16 ms step
// (f10 = 0 on the others takes the ble and skips the damage).
void Q4Tick_WalkerStomp(PPCRegister& f10) {
  if (!q4tick::Fast()) return;
  if (!q4tick::OnOriginalFrameBoundary()) {
    f10.f64 = 0.0;
    return;
  }
  const double s = 1.0 / q4tick::Frac();
  f10.f64 = double(float(f10.f64 * s * s));
}

// VehicleParts.cpp:1934-1935  rvVehicleHoverpad::RunPhysics (0x8256A908):
//   dampingForce = (dampen * ((velocity * d) / (d * d))) * d;  ... impulseForce = ... - dampingForce;
// The hover force is msec-scaled but the damping impulse is a per-frame velocity-proportional
// impulse.
//   8256ae74  lfs f4,140(r31)           ; f4 = dampen                <- hook after
//   8256af24  fmuls f11,f11,f4          ; only use of f4
void Q4Tick_HoverpadDampen(PPCRegister& f4) {
  if (!q4tick::Fast()) return;
  f4.f64 = double(float(f4.f64 * q4tick::Frac()));
}

// VehicleParts.cpp:2223  rvVehicleThruster::RunPhysics (0x8256CBC0):
//   parent->GetPhysics()->ApplyImpulse(0, worldOrigin, worldAxis[forceAxis] * force * mult);
// A fixed impulse per frame.
//   8256cd08  lfs f0,136(r31)           ; f0 = force                 <- hook after
//   8256cd3c..48 fmuls f13/f12/f0 = axis * f0 -> impulse (the 8256ce14 reload is debug-draw only)
void Q4Tick_ThrusterForce(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(f0.f64 * q4tick::Frac()));
}

// =============================================================================================
// Player
// =============================================================================================

// #10  idPlayer::Move (0x823C2B48), Player.cpp:9031:
//   SetEyeHeight(EyeHeight() * pm_crouchrate + newEyeOffset * (1 - pm_crouchrate));   (0.87)
//   823c3024  lfs    f0,40(r11)         ; f0 = pm_crouchrate
//   823c3028  fsubs  f13,f13,f0         ; f13 = 1 - rate             <- hook before
//   823c302c  fmuls  f12,f0,f12         ; rate * EyeHeight
//   823c3030  fmadds f1,f13,f1,f12
void Q4Tick_CrouchRate(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = SafePowFrac(f0.f64);
}

// Player.cpp:9059  idPlayer::Move (0x823C2B48): standing on an AI, vel *= 1.1f per frame.
//   823c3274  lfs   f0,-4168(r11)       ; f0 = 1.1
//   823c3278  fmuls f6,f13,f0           <- hook before (f13,f12,f4 = vel.xyz)
//   823c3280  fmuls f5,f12,f0 / 823c3288 fmuls f3,f4,f0
void Q4Tick_MonsterPush(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = SafePowFrac(f0.f64);
}

// Player.cpp:7787  idPlayer::UpdateViewAngles (0x823BFF70), INFLUENCE_LEVEL3:
//   viewAngles[i] += ClampFloat(-1, 1, AngleDelta(...));   1 degree per frame cap
//   823c01a8  ...                       ; f1 = AngleDelta (unclamped)
//   823c01ac..bc fsubs/fsel/fsubs/fsel  ; f0 = Clamp(-1, 1, f1)
//   823c01c0  fadds f3,f6,f0            <- hook before (f0 replaced by Clamp(-k, k, f1))
void Q4Tick_InfluenceViewClamp(PPCRegister& f0, PPCRegister& f1) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::Frac();
  const double d = f1.f64;
  if (d != d) return;  // NaN: keep the original result
  f0.f64 = double(float(std::clamp(d, -k, k)));
}

// Player.cpp:7866, 7886  idPlayer::UpdateAir (0x823C03C0): airTics-- / airTics += 2 per frame
// (pm_airTics 1800 = 30 s at 60 Hz). Count only on tics that end a 16 ms step.
//   823c0520  lwz    r6,7560(r31)       ; airTics
//   823c0524  addic. r11,r6,-1          <- hook before (r6 += 1 on the other tics: no change)
//   823c0528  stw    r11,7560(r31) ; 823c052c bge 0x823c06a8 (cr0 from the addic.)
//   823c0688  lwz    r11,7560(r31)
//   823c068c  addi   r11,r11,2          <- hook before (r11 -= 2 on the other tics: no change)
// The "no air" damage after airTics < 0 is itself time-gated (lastAirDamage + delay).
void Q4Tick_AirLose(PPCRegister& r6) {
  if (!q4tick::Fast() || q4tick::OnOriginalFrameBoundary()) return;
  r6.s64 = int64_t(int32_t(r6.u32) + 1);
}
void Q4Tick_AirRegain(PPCRegister& r11) {
  if (!q4tick::Fast() || q4tick::OnOriginalFrameBoundary()) return;
  r11.s64 = int64_t(int32_t(r11.u32) - 2);
}

// Player.cpp:10506-10533  idPlayer::GunTurningOffset (inlined, 0x823C73C8): averages the last
// weaponAngleOffsetAverages (10) entries of loggedViewAngles[framenum & 63], one per frame.
// Use twice as many tics so the averaging window stays 160 ms (buffer holds 64).
//   823c7418  lwz r7,1680(r11)          ; r7 = weapon->weaponAngleOffsetAverages  <- hook after
//   r7 is the loop count (823c7444, 823c748c, 823c76c4) and the 1/N divisor (823c7488, 823c76cc).
void Q4Tick_GunTurningAverages(PPCRegister& r7) {
  if (!q4tick::Fast()) return;
  const int n = int32_t(r7.u32);
  if (n <= 0) return;
  const int scaled = std::min(64, (n * 16) / q4tick::Msec());
  r7.u64 = uint64_t(uint32_t(scaled));
}

// Player.cpp:9515  idPlayer::Think (0x823C3E78): dynamicProtectionScale += 0.05f per frame.
//   823c4754  lfs   f0,-30720(r11)      ; 0.05
//   823c4758  fadds f0,f13,f0           <- hook before
void Q4Tick_DynamicProtection(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(f0.f64 * q4tick::Frac()));
}

// Player.cpp:9124-9143  idPlayer::UpdateIntentDir (0x823C3570) -> BiasIntentDir(newDir, prevBias):
//   intentDir = (intentDir * prevBias + newDir) / (prevBias + 1)   once per frame (199/39/19)
// The per-frame blend rate is r = 1/(prevBias+1); convert it to a per-tic rate.
// (BiasIntentDir is also called on weapon fire; only this per-frame call site is changed.)
//   823c3718  fmr f1,f30                ; f1 = prevBias              <- hook after
//   823c3720  bl  0x823c33f8            ; idPlayer::BiasIntentDir
void Q4Tick_IntentDirBias(PPCRegister& f1) {
  if (!q4tick::Fast()) return;
  const double b = f1.f64;
  if (!(b > 0.0)) return;
  const double r = q4tick::RateFrac(1.0 / (b + 1.0));
  f1.f64 = double(float(1.0 / r - 1.0));
}

// Player.cpp:8829  idPlayer::UpdateAnimation-legs (0x823C22B8):
//   legsYaw = legsYaw * 0.9f + idealLegsYaw * 0.1f;   (3rd-person legs, cosmetic)
//   823c2664  lfs    f11,7420(r31)      ; idealLegsYaw
//   823c266c  fmuls  f10,f11,f29        ; f10 = ideal * 0.1
//   823c2674  lfs    f0,-30252(r11)     ; f0 = 0.9
//   823c2678  fmadds f8,f9,f0,f10       <- hook before
void Q4Tick_LegsYawBlend(PPCRegister& f0, PPCRegister& f10, PPCRegister& f11) {
  if (!q4tick::Fast()) return;
  const double k = f0.f64;
  if (!(k > 0.0 && k < 1.0)) return;
  const double k2 = q4tick::PowFrac(k);
  // f10 = ideal * (1 - k) originally; use the per-tic rate instead.
  f10.f64 = double(float(f11.f64 * (1.0 - k2)));
  f0.f64 = double(float(k2));
}

// =============================================================================================
// Physics
// =============================================================================================

// #11  idPhysics_Player::SlideMove (0x825112F8), Physics_Player.cpp:301:
//   current.velocity *= 1.0f - ClampFloat(0, 1000, totalMass - 20) * (1/950);   per frame
//   825119f4  fnmsubs f0,f0,f16,f28     ; f0 = 1 - clamp * (1/950)   <- hook after
//   825119f8..0c fmuls f4/f3/f2 = f0 * velocity.xyz
void Q4Tick_PushVelocityFactor(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(SafePowFrac(f0.f64)));
}

// #14  idPhysics_RigidBody::ContactFriction (0x8242D5C8), Physics_RigidBody.cpp:235:
//   impulseNumerator = contactFriction * magnitude;
// Called once per idPhysics_RigidBody::Evaluate (one call per game frame, deltaTime = 0.008 at
// 120 Hz but unused). The impulse removes the fraction contactFriction of the tangential contact
// velocity each frame.
//   8242d860  lfs   f17,436(r31)        ; f17 = contactFriction (reloaded per contact)
//   8242d86c  fmuls f23,f17,f23         <- hook before (f23 = magnitude)
// deltaTime (f1 on entry) is overwritten at 8242d6b0, so the tic fraction is used.
void Q4Tick_RigidBodyContactFriction(PPCRegister& f17) {
  if (!q4tick::Fast()) return;
  f17.f64 = double(float(SafeRateFrac(f17.f64)));
}

// #15  idPhysics_AF::Evolve (0x82422D60), Physics_AF.cpp:5328-5329, 5376-5377.
// Evolve is called once per idPhysics_AF::Evaluate (once per game frame, timeStep = 0.008 x timescale).
//   maxLinearVelocity  = af_maxLinearVelocity  / timeStep;   (cap is a per-step distance)
//   maxAngularVelocity = af_maxAngularVelocity / timeStep;
//   82422da0  fdivs f0,f21,f31          ; 1 / timeStep
//   82422db4  fmuls f30,f13,f0          ; maxLinearVelocity
//   82422dc0  fmuls f29,f12,f0          ; maxAngularVelocity          <- hook after
//   (f30/f29 are only used as the caps in the first body loop, 82422eb4..82422f58)
void Q4Tick_AFMaxVelocity(PPCRegister& f30, PPCRegister& f29) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::Frac();
  f30.f64 = double(float(f30.f64 * k));
  f29.f64 = double(float(f29.f64 * k));
}
//   vel -= body->linearFriction * vel;  vel -= body->angularFriction * angVel;   per step
//   82423168  lfs f0,88(r31)            ; body->linearFriction        <- hook after
//   8242319c..ac fmuls f13/f12/f0 = vel * f0
//   824231d0  lfs f0,92(r31)            ; body->angularFriction       <- hook after
void Q4Tick_AFLinearFriction(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(SafeRateFrac(f0.f64)));
}
void Q4Tick_AFAngularFriction(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(SafeRateFrac(f0.f64)));
}

// Force_Field.cpp:251-258  idForce_Field::Evaluate (0x82594A78), FORCEFIELD_APPLY_IMPULSE:
//   entity->ApplyImpulse(world, id, point, force * magnitude)  once per Think (per frame)
//   82594e2c  beq 0x82594f3c (APPLY_VELOCITY) ; 82594e28 blt (APPLY_FORCE) -> impulse falls through
//   82594e50  lfs f0,12(r31)            ; f0 = magnitude              <- hook after
//   used at 82594e8c..9c (random torque path) and 82594f04..14 (plain path)
void Q4Tick_ForceFieldImpulse(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = double(float(f0.f64 * q4tick::Frac()));
}
