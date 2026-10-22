// q4tick group "ai": AI / monster per-frame timing fixes for the 120 Hz tic rate.
// Hook entries: hooks_ai.toml. Every hook is a no-op when !q4tick::Fast().
//
// Two kinds of fix are used:
//  - "once per original frame" logic (round-robin, random rolls, state steps) runs only on tics
//    for which q4tick::OnOriginalFrameBoundary() is true, i.e. once per 16 ms of game time;
//  - per-frame rates/factors/steps held in FPRs are converted to one 8 ms tic
//    (RateFrac / PowFrac / * Frac()).
#include "q4_tick.h"

namespace {
// The FPR feeds single-precision arithmetic (fmuls/fmadds); keep it representable as a float.
inline double AsFloat(double v) { return double(float(v)); }
inline bool SkipThisTic() { return q4tick::Fast() && !q4tick::OnOriginalFrameBoundary(); }
}  // namespace

// ---------------------------------------------------------------------------------------------
// Audit #2: AI complex-think round-robin.
// SDK ai/AI_Manager.cpp:46-49 (rvAIManager::RunFrame pops the head of simpleThink every frame) and
// ai/AI_Manager.cpp:327-347 (rvAIManager::IsSimpleThink: only the list head gets a complex think),
// called from ai/AI.cpp:1146. With both hooks the head advances once per 16 ms and nobody gets a
// complex think on the in-between tic, so each AI's complex-think period, its pain accumulation
// window (AI.cpp:1560/3625), FindEnemy notice roll (AI.cpp:4206), UpdateTactical (AI_States.cpp:1611)
// and action selection (AI_Actions.cpp:436) are back at their 60 Hz cadence.
// ---------------------------------------------------------------------------------------------

// rvAIManager::RunFrame @0x8249E5A0:
//   8249e5f0 cmplw cr6,r6,r11 ; ... 8249e600 cmplwi cr6,r4,0 ; 8249e604 bne cr6,0x8249e650 (list empty)
//   8249e608 lwz r10,128(r22) ... 8249e64c stw r11,0(r11)   <- inline idLinkList::Remove of the head
//   8249e650 (ai_speeds print etc.)
// Hook before 0x8249E608; returning true jumps to 0x8249E650 (skip the pop).
bool Q4Tick_AIRoundRobinPop(PPCRegister& r22) {
  (void)r22;
  return SkipThisTic();
}

// idAI::Think @0x82527590 (inlined IsSimpleThink):
//   82527654 lwz r10,8(r11) ; 82527658 cmplwi r10,0 ; 8252765c beq 0x82527678   (prev == NULL)
//   82527660 lwz r4,0(r11)  ; 82527664 cmplw r10,r4 ; 82527668 beq 0x82527678   (prev == list head)
//   82527678 li r11,0  <- "complex think" (only reached from the head-of-list test, not from the
//                         ai_disableSimpleThink path at 825275e4)
//   82527684 clrlwi r3,r11,24 ; 8252768c rlwimi r11,r3,3,28,28 ; 82527690 stb r11,7745(r31) (aifl.simpleThink)
// Hook after 0x82527678: on the in-between tic the head also simple-thinks.
void Q4Tick_AIForceSimpleThink(PPCRegister& r11) {
  if (SkipThisTic()) r11.u64 = 1;
}

// ---------------------------------------------------------------------------------------------
// Audit #8: idActor::DeathPush @0x8247EF38, SDK Actor.cpp:2604-2608.
// ApplyImpulse(0, point, deathPushForce) runs every frame until deathPushTime; scale the force by
// msec/16 so the total momentum over the push time is unchanged.
//   joint path: 8247f0bc lwz r3,692(r31) ; 8247f0c0 addi r6,r31,4936 (&deathPushForce) ; ... 8247f0d8 bctrl
//   else path : 8247f0e4 lwz r3,692(r31) ; 8247f0e8 addi r6,r31,4936 ; ... 8247f100 bctrl
// Hooks after the addi: copy the scaled vector into the stack slot r1+120..131, which held the
// anti-gravity impulse (8247f070-8247f080 stfs ...,120/124/128(r1)) already consumed by the first
// ApplyImpulse at 8247f090, and point r6 at it. deathPushForce itself is left untouched.
// ---------------------------------------------------------------------------------------------
void Q4Tick_DeathPushForce(PPCRegister& r1, PPCRegister& r6) {
  if (!q4tick::Fast() || q4tick::Base() == nullptr) return;
  const uint32_t src = r6.u32;
  const uint32_t dst = r1.u32 + 120;
  const float k = float(q4tick::Frac());
  for (uint32_t i = 0; i < 3; ++i) q4tick::StF(dst + 4 * i, q4tick::LdF(src + 4 * i) * k);
  r6.u64 = dst;
}

// ---------------------------------------------------------------------------------------------
// Audit #4: rvMonsterLightTank::State_Torso_PowerUp @0x8259C9D0, STAGE_LOOP, SDK
// ai/Monster_LightTank.cpp:594 "health++" every frame for up to 3875 ms.
//   8259caf0 lwz r11,200(r31) ; 8259caf4 addi r8,r11,1 ; 8259caf8 stw r8,200(r31)
// Hook after 0x8259CAF4: only add the point once per 16 ms (exactly the 60 Hz regen rate).
// ---------------------------------------------------------------------------------------------
void Q4Tick_LightTankPowerUpHealth(PPCRegister& r8, PPCRegister& r11) {
  if (SkipThisTic()) r8.u64 = r11.u64;
}

// ---------------------------------------------------------------------------------------------
// Audit #12: idAI::UpdateEnemy @0x82529F10, SDK ai/AI.cpp:2077-2083 enemy velocity smoothing used for
// lead aim (AI.cpp:2377): s += (v - s) * smoothVelocityRate (default 0.1) every frame.
//   8252a2b4 lfs f0,8140(r31) -> fmuls f12/f13/f0 (smoothedLinearVelocity, visible)
//   8252a340 lfs f0,8140(r31) -> fmuls (smoothedPushedVelocity, visible)
//   8252a394 lfs f0,8140(r31) -> fmuls (smoothedLinearVelocity decay, not visible)
//   8252a3e4 lfs f0,8140(r31) -> fmuls (smoothedPushedVelocity decay, not visible)
// Hooks after each lfs: rate -> 1 - (1 - rate)^(msec/16).
// ---------------------------------------------------------------------------------------------
void Q4Tick_AIEnemyVelSmooth(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = AsFloat(q4tick::RateFrac(f0.f64));
}

// ---------------------------------------------------------------------------------------------
// idAI::UpdateAnimationControllers @0x8252D8B8, SDK ai/AI.cpp:3151 and 3207-3224: head look angles
// lerp by currentHeadFocusRate (head_focus_rate 0.06, halved when not aware) every frame.
//   8252d8f0 lfs f25,7700(r30) ; 8252d8f4 b 0x8252d900           (aware)
//   8252d8f8 lfs f0,7700(r30)  ; 8252d8fc fmuls f25,f0,f21(0.5)  (not aware)
//   8252d900 ...  f25 is then only read by fmadds at 8252dd18/8252dd54/8252dd7c.
// Hook before 0x8252D900 (join point, runs once per call).
// The eye rate (eye_focus_rate, lfs f30,7704(r30) at 8252ddc8) is deliberately NOT changed: see
// notes_ai.md (the per-frame eye tweak at AI.cpp:3345-3351 would then be over-applied).
// ---------------------------------------------------------------------------------------------
void Q4Tick_AIHeadFocusRate(PPCRegister& f25) {
  if (!q4tick::Fast()) return;
  f25.f64 = AsFloat(q4tick::RateFrac(f25.f64));
}

// idAI::UpdateAnimationControllers, SDK ai/AI.cpp:3286 look-joint blend back "curJointAngleMod *= 0.75f"
// every frame (comment in the SDK admits it is frame-rate dependent).
//   8252dfd8 lis r11,-32242 ; 8252dfe0 lfs f31,-30832(r11)  (0x820D8790 = 0.75, loop invariant)
//   8252e064/8252e074/8252e078 fmuls ...,f31 ; f31 is not otherwise used until reloaded at 8252e658.
// Hook after 0x8252DFE0: factor -> 0.75^(msec/16).
void Q4Tick_AILookJointBlendBack(PPCRegister& f31) {
  if (!q4tick::Fast()) return;
  f31.f64 = AsFloat(q4tick::PowFrac(f31.f64));
}

// ---------------------------------------------------------------------------------------------
// idAI::AdjustFlyingAngles @0x824A8D38, SDK ai/AI_Move.cpp:2269-2270
// fly_roll = fly_roll*0.95 + roll*0.05 ; fly_pitch likewise, every frame.
//   824a8e50 lfs f13,-12840(r11) (0x820DCDD8 = 0.95) ; 824a8e58 lfs f12,-30720(r11) (0x820D8800 = 0.05)
//   824a8e5c fmuls f11,f10,f12 ; 824a8e64 fmuls f9,f10,f13 ; 824a8e68 fmadds f13,f7,f13,f11 ;
//   824a8e70 fmadds f0,f0,f12,f9
// Hook after 0x824A8E58: keep factor + rate = 1 with factor = 0.95^(msec/16).
// ---------------------------------------------------------------------------------------------
void Q4Tick_AIFlyBank(PPCRegister& f12, PPCRegister& f13) {
  if (!q4tick::Fast()) return;
  f13.f64 = AsFloat(q4tick::PowFrac(f13.f64));
  f12.f64 = AsFloat(q4tick::RateFrac(f12.f64));
}

// ---------------------------------------------------------------------------------------------
// Audit #13: rvMonsterConvoyHover::Think @0x8257ACC8 (labelled idAFEntity_Base::Think_8257ACC8),
// SDK ai/Monster_ConvoyHover.cpp:237-299 + Think_Random 305-351. Everything after the driver check
// is per frame: flyclip ApplyImpulse(vel*2) and desiredHeight/distance +-5 (253-262), CanSee +-5,
// Think_Random impulses 16000/9000 and the 5% re-target roll, the 8-sample CalcDampening moving
// average (477-493) and SetAxis(GetAxis()*hover).
//   8257ad14 bne cr6,0x8257ad34                 (driver valid)
//   8257ad18 mr r3,r31 ; 8257ad1c bl 0x8259d838 (rvVehicleMonster::Think) ; 8257ad20.. epilogue, return
// Hook before 0x8257AD34: on the in-between tic jump to 0x8257AD18, so only rvVehicleMonster::Think
// runs (physics, animation) and the hover's own steering/tilt runs once per 16 ms as at 60 Hz.
// ---------------------------------------------------------------------------------------------
bool Q4Tick_ConvoyHoverSkipTic(PPCRegister& r31) {
  (void)r31;
  return SkipThisTic();
}

// ---------------------------------------------------------------------------------------------
// rvMonsterStroggHover::State_Torso_BombAttack @0x825B61A8, SDK ai/Monster_StroggHover.cpp:1062-1067
// if (vel.z < 150) vel.z += 20 every frame.
//   825b6204 lfs f30,-48(r29) (150) ; 825b620c bge ... ; 825b6214 lfs f0,172(r29) (0x82060F94 = 20.0)
//   825b6218 fadds f0,f13,f0 ; 825b6220 stfs f0,104(r1)
// Hook after 0x825B6214: 20 -> 20*msec/16.
// ---------------------------------------------------------------------------------------------
void Q4Tick_StroggHoverBombClimb(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = AsFloat(f0.f64 * q4tick::Frac());
}

// rvMonsterStroggHover::State_DeathSpiral @0x825B7570, STAGE_SPIRAL, SDK Monster_StroggHover.cpp:1419-1440:
// yaw/pitch/roll += rate, velocity re-aimed and += gravity/deathGrav, frequencyShift += 0.025, per frame.
// (Review fix: the earlier version skipped the whole stage on in-between tics. idAI::Think resets
// viewAxis = idAngles(0, current_yaw, 0) before UpdateStates every think (0x82527590, the
// param_1[0x2b6..0x2be] copy), and this stage is what puts deathPitch/deathRoll back into viewAxis,
// so a skipped tic rendered the craft level every other tic. Instead the four per-frame steps are
// scaled by msec/16 and the stage runs every tic.)
//   825b75a8 lfs f0,10096(r31) (deathYawRate) ; 825b75b4 fneg f0 (strafeRight) ;
//   825b75b8 (join, branch target of 825b75b0) ; 825b75d0 fadds f0,f0,f9 -> stfs 5104 (current_yaw)
//   825b75c0 lfs f13,10092(r31) (deathPitchRate) ; 825b75c8 fadds f13,f13,f10 (deathPitch + rate)
//   825b760c lfs f12 / 825b7618 fneg f12 (+-deathRollRate) ; 825b761c (join) ; 825b762c fadds f0,f5,f12
//   825b77e0 lfs f0,-21584(r11) (0.025) ; 825b77e4 fadds f12,f13,f0 -> stfs 564(r31) (frequencyShift)
// Hooks: before 0x825B75B8 (f0), after 0x825B75C0 (f13), before 0x825B761C (f12), after 0x825B77E0
// (f0). The velocity re-aim (normalize(vel + strafe) * speed + gravity/deathGrav) overwrites the
// velocity each frame, so its steady state does not depend on the rate and is left alone.
void Q4Tick_StroggHoverDeathSpiralStep(PPCRegister& f) {
  if (!q4tick::Fast()) return;
  f.f64 = AsFloat(f.f64 * q4tick::Frac());
}

// ---------------------------------------------------------------------------------------------
// rvAIMedic::State_Medic treating stage @0x8253B3C0 (labelled CLASS_STATES_DECLARATION),
// SDK ai/AI_Medic.cpp:806-818: if (RandomFloat() > 0.5) patient->health++ / armor++ every frame.
//   8253bb04 fmuls f9,f10,f0 ; 8253bb08 lfs f0,3828(r11) (0.5) ; 8253bb0c fcmpu cr6,f9,f0
//   8253bb10 ble cr6,0x8253bc44 ; 8253bb14 lbz r10,9888(r31) (isTech) ... heal
//   8253bc44 li r3,4 (SRESULT_WAIT) ; epilogue
// Hook before 0x8253BB14: on the in-between tic take the "no heal" exit.
// ---------------------------------------------------------------------------------------------
bool Q4Tick_MedicHealTick(PPCRegister& r31) {
  (void)r31;
  return SkipThisTic();
}

// ---------------------------------------------------------------------------------------------
// idPlayer::Think @0x823C3E78, SDK Player.cpp:9513-9519 (single player, g_useDynamicProtection):
// dynamicProtectionScale += 0.05 every frame once 500 ms passed since the last damage.
//   823c4728 lfs f13,8784(r31) ; ... 823c4754 lfs f0,-30720(r11) (0x820D8800 = 0.05)
//   823c4758 fadds f0,f13,f0 ; 823c475c stfs f0,8784(r31) ; clamp to 1.0 (f31)
// Hook after 0x823C4754: 0.05 -> 0.05*msec/16.
// ---------------------------------------------------------------------------------------------
void Q4Tick_DynamicProtectionRecover(PPCRegister& f0) {
  if (!q4tick::Fast()) return;
  f0.f64 = AsFloat(f0.f64 * q4tick::Frac());
}

// ---------------------------------------------------------------------------------------------
// rvMonsterConvoyGround::State_Legs_Move @0x8255D620, SDK ai/Monster_ConvoyGround.cpp:591
// moveCurrentAnimRate += (rate - moveCurrentAnimRate) * moveAccelRate (0.1) every frame.
//   8255d7f0 lfs f13,9568(r31) ; 8255d7f8 fsubs f11,f0,f13 ; 8255d804 lfs f12,9580(r31) (moveAccelRate)
//   8255d810 fmadds f1,f11,f12,f13 ; 8255d814 stfs f1,9568(r31)
// Hook after 0x8255D804: rate -> 1 - (1 - rate)^(msec/16).
// ---------------------------------------------------------------------------------------------
void Q4Tick_ConvoyGroundAnimRate(PPCRegister& f12) {
  if (!q4tick::Fast()) return;
  f12.f64 = AsFloat(q4tick::RateFrac(f12.f64));
}
