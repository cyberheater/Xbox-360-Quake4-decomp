// q4tick group "engine": weapon refire quantisation, engine tic-loop glue, camera animations,
// playback blend-in and controller look smoothing for the 120 Hz tic rate.
// Hook entries: hooks_engine.toml. Notes: notes_engine.md. Every hook is a no-op when !q4tick::Fast().
#include "q4_tick.h"

namespace {
inline bool Ready() { return q4tick::Fast() && q4tick::Base() != nullptr; }
inline double AsFloat(double v) { return double(float(v)); }

// ---------------------------------------------------------------------------------------------
// 16 ms refire quantisation.
//
// At 60 Hz every tic lands on a multiple of 16 ms, so "nextAttackTime = time + R" followed by a
// per-tic test fires again at
//   time >= N : t + ceil16(R)                (held-trigger loops, Think-driven damage ticks)
//   time >  N : t + floor16(R) + 16          (idle-state re-fire, gauntlet, vehicle guns, ...)
// At 8 ms tics both tests would fire up to 8 ms earlier (MG 112->104 ms, LG 64->56 ms, ...).
// Rewriting N after it is stored restores the 60 Hz interval:
//   GE sites: N = t + ceil16(R) - 1 when R is not a multiple of 16. Then ">=" fires at
//             t + ceil16(R) and ">" fires at t + ceil16(R) as well, both exactly as at 60 Hz.
//             (R a multiple of 16 is left alone: ">=" is already exact, ">" fires 8 ms early;
//             one integer cannot satisfy both tests, and the held-trigger ">=" path is the one
//             that sets the rate of fire.)
//   GT sites (only ">" tests follow): N = t + floor16(R) + 15 -> fires at t + floor16(R) + 16.
// t is the gameLocal.time the interval was added to (all sites add to gameLocal.time).
// ---------------------------------------------------------------------------------------------
inline int RefireDeadline(int t, int n, bool gtOnly) {
  const int r = n - t;
  if (r <= 0) return n;
  if (gtOnly) return t + (r / 16) * 16 + 15;
  if (r % 16 == 0) return n;
  return t + ((r + 15) / 16) * 16 - 1;
}
inline void FixStoredDeadline(uint32_t addr, bool gtOnly) {
  if (!Ready() || addr == 0) return;
  const int t = q4tick::GameTime();
  const int n = int(q4tick::Ld32(addr));
  const int fixed = RefireDeadline(t, n, gtOnly);
  if (fixed != n) q4tick::St32(addr, uint32_t(fixed));
}
// Same rule for a deadline still in a register (hook before the store); t in a register too.
inline void FixDeadlineReg(PPCRegister& n, const PPCRegister& t, bool gtOnly) {
  if (!q4tick::Fast()) return;
  n.s64 = int64_t(RefireDeadline(t.s32, n.s32, gtOnly));
}
// "elapsed > D" threshold test on a duration register: elapsed is a multiple of 16 at 60 Hz, so
// the test passed at floor16(D) + 16; D' = floor16(D) + 15 gives the same at 8 ms steps.
inline void FixGtThreshold(PPCRegister& d) {
  if (!q4tick::Fast() || d.s32 < 0) return;
  d.s64 = int64_t((d.s32 / 16) * 16 + 15);
}
constexpr uint32_t kNextAttackTime = 0xCC;  // rvWeapon::nextAttackTime (r31 = rvWeapon*)
}  // namespace

// ---- Audit #9: player weapons, rvWeapon::nextAttackTime (+0xCC) ------------------------------
// Each hook runs after the store; the stored word is re-read and rewritten.
// All of these compute (int)(fireRate * owner->PowerUpModifier(PMOD_FIRERATE) + (float)time):
//   ... fmadds fX,f(rate),f(mod),f(time) ; fctiwz fY,fX ; stfiwx fY,0,rA   (rA = addi r31,204)

// rvWeaponMachinegun::State_Fire @0x8256E350 (WeaponMachinegun.cpp:230/234, test :241 ">=",
// idle :194/:199 ">"): 8256e584 addi r9,r31,204 ... 8256e5c8 fctiwz f6,f7 ; 8256e5cc stfiwx f6,0,r9
// and 8256e6ac addi r11,r31,204 ... 8256e6d0 fctiwz f8,f9 ; 8256e6d4 stfiwx f8,0,r11.
// rvWeaponRocketLauncher::State_Fire (RL.cpp:448, :454 ">="): 82589c58 addi r11,r31,204 ; 82589c80 stfiwx f6,0,r11
// rvWeaponGrenadeLauncher::State_Fire (GL.cpp:147, :153 ">="): 8258f934 addi r10,r31,204 ; 8258f960 stfiwx f6,0,r10
// rvWeaponNailgun::State_Fire (Nailgun.cpp:671/674, :690 ">="): 825923e8 addi r4,r31,204 ; 8259241c stfiwx f2,0,r4
//                                                               825924cc addi r4,r31,204 ; 82592500 stfiwx f7,0,r4
// rvWeaponShotgun::State_Fire (Shotgun.cpp:166, :176 ">="): 8259ec7c addi r8,r31,204 ; 8259ecc0 stfiwx f6,0,r8
// rvWeaponHyperblaster::State_Fire (HB.cpp:231, :242 ">="): 825af700 addi r10,r31,204 ; 825af72c stfiwx f6,0,r10
// (the address register is not redefined between the addi and the stfiwx at any of these sites)
void Q4Tick_WeaponNextAttackGE(PPCRegister& addr) { FixStoredDeadline(addr.u32, false); }

// rvWeaponDarkMatterGun::State_Fire (DMG.cpp:316; SP re-fire only via State_Idle :283 ">"):
//   82576364 addi r9,r31,204 ... 8257638c fctiwz f6,f7 ; 82576390 stfiwx f6,0,r9
// rvWeaponRailgun::State_Fire (Railgun.cpp:189; SP re-fire only via State_Idle :159 ">"; the
// ">=" at :195 is multiplayer-only): 8259e2f4 addi r9,r31,204 ; 8259e320 stfiwx f6,0,r9
void Q4Tick_WeaponNextAttackGT(PPCRegister& addr) { FixStoredDeadline(addr.u32, true); }

// rvWeaponBlaster::UpdateAttack @0x82583670 (WeaponBlaster.cpp:106-109, ">="):
//   825837a8 fctiwz f6,f7 ; 825837ac li r12,204 ; 825837b0 stfiwx f6,r31,r12
// rvWeaponLightningGun::Think @0x82585... (WeaponLightningGun.cpp:309-319, ">="; idle :797 ">"):
//   825856b4 fctiwz f13,f0 ; 825856b8 li r12,204 ; 825856bc stfiwx f13,r31,r12
void Q4Tick_WeaponNextAttackGE_R31(PPCRegister& r31) {
  FixStoredDeadline(r31.u32 + kNextAttackTime, false);
}

// rvWeaponGauntlet blade damage (WeaponGauntlet.cpp:283 "time > nextAttackTime", :301
// nextAttackTime = time + fireRate) in the function at 0x82556018:
//   825564e8 lwz r4,204(r31) ; 825564f4 cmpw cr6,r10,r4 ; 825564f8 ble ...
//   82556684 lwz r11,192(r31) ; 8255668c lwzx r10,r30,r9 (time) ; 82556690 add r8,r11,r10 ; 82556694 stw r8,204(r31)
void Q4Tick_GauntletNextAttack(PPCRegister& r31) {
  FixStoredDeadline(r31.u32 + kNextAttackTime, true);
}

// rvWeaponBlaster::UpdateAttack (WeaponBlaster.cpp:118) "time - fireHeldTime > chargeDelay":
//   82583814 lwz r10,1940(r31) (chargeDelay) ; 82583818 subf r9,r11,r8 ; 8258381c cmpw cr6,r9,r10 ; 82583820 ble
// Hook before 0x8258381C. r10 is not read again on either path. chargeDelay 100 -> 111: charge
// starts after 112 ms held, as at 60 Hz (104 ms otherwise).
void Q4Tick_BlasterChargeDelay(PPCRegister& r10) { FixGtThreshold(r10); }

// rvWeaponBlaster::State_Fire (WeaponBlaster.cpp:429) "time - fireHeldTime > chargeTime" picks the
// charged shot: 82584444 lwz r8,1936(r31) (chargeTime) ; 82584464 subf r4,r7,r11 ;
// 82584468 cmpw cr6,r4,r8 ; 8258446c ble. Hook before 0x82584468; r8 is rewritten (li/lwz) on both
// paths before any further read. chargeTime 800 -> 815: charged after 816 ms held, as at 60 Hz.
void Q4Tick_BlasterChargedShot(PPCRegister& r8) { FixGtThreshold(r8); }

// rvDarkMatterProjectile::Think @0x82576890 (WeaponDarkMatterGun.cpp:464-466): radius damage
// "if (time > nextDamageTime) { RadiusDamage; nextDamageTime = time + SEC2MS(damageRate) }",
// damageRate .05 -> 64 ms at 60 Hz, 56 ms (+14% DPS) at 120 Hz without the fix.
//   825768d0 lwz r9,1504(r31) ; 825768e0 cmpw cr6,r11,r9 ; 825768e4 ble (test)
//   825769f4 lwzx r11,r30,r5 (time) ; ... 82576a04 subf r4,r7,r11 ; 82576a08 stw r4,1504(r31)
// (the constant at 0x820E8754 is negative, so subf yields time + rate). Hook before 0x82576A08.
void Q4Tick_DarkMatterDamageRate(PPCRegister& r4, PPCRegister& r11) { FixDeadlineReg(r4, r11, true); }

// rvVehicleWeapon::RunPostPhysics @0x825652A8 (VehicleParts.cpp:893 "time <= nextFireTime ->
// return", :909 nextFireTime = time + fireDelay): GEV .1/.3/1.75, walker .1/.3/.6, flatbed .09,
// phatride .07/1.2, tramgun .05.
//   8256539c lwz r6,144(r31) ... (test)
//   82565490 lwz r10,148(r31) (fireDelay) ; 82565498 lwzx r11,r30,r5 (time) ; 8256549c add r4,r10,r11 ;
//   825654a0 stw r4,144(r31)
// Hook before 0x825654A0.
void Q4Tick_VehicleWeaponNextFire(PPCRegister& r4, PPCRegister& r11) { FixDeadlineReg(r4, r11, true); }

// rvMonsterStroggMarine::State_Torso_MovingRangedAttack @0x825B2488 (Monster_StroggMarine.cpp:607
// nextShootTime = time + attackRate (0.2 default, no def override), :635 "time >= nextShootTime"):
// 208 ms between shots at 60 Hz, 200 ms at 120 Hz without the fix.
//   825b24c4 lwz r10,9580(r31) ; 825b24d8 cmpw cr6,r11,r10 ; 825b24dc blt (test)
//   825b256c lwzx r10,r11,r10 (time) ; 825b2570 add r9,r9,r10 ; 825b2574 stw r9,9580(r31)
// Hook before 0x825B2574.
void Q4Tick_StroggMarineNextShoot(PPCRegister& r9, PPCRegister& r10) { FixDeadlineReg(r9, r10, false); }

// ---- Engine glue: idSessionLocal::Frame tic clamp (Session.cpp:2697-2699) --------------------
// idSessionLocal::Frame_2 @0x821B6D80:
//   821b736c lwz r8,2640(r30) (latchedTicNumber) ; 821b7370 lwz r7,2644(r30) (lastGameTic) ;
//   821b7378 subf r29,r7,r8 (numCmdsToRun) ; [timeHitch skip 821b7384-821b73c4, r29 -= skip]
//   821b73c8 cmpwi cr6,r29,10 ; 821b73cc ble 0x821b73dc ;
//   821b73d0 lwz r11,2640(r30) ; 821b73d4 addi r5,r11,-10 ; 821b73d8 stw r5,2644(r30)
//   821b73dc lwz r4,12(r30) ; 821b73e0 cmplwi cr6,r4,0 ...   (cr6 recomputed)
// Hook before 0x821B73C8 (reached from both timeHitch paths); returns true -> jump to 0x821B73DC.
// The limit becomes 10 * 16 / msec tics (20 at 120 Hz) = 160 ms of game time as at 60 Hz. r29 keeps
// its value, as in the original (the clamp only rewrites lastGameTic).
bool Q4Tick_SessionTicClamp(PPCRegister& r29, PPCRegister& r30) {
  if (!Ready()) return false;
  const int limit = 10 * 16 / q4tick::Msec();
  if (r29.s32 > limit) {
    const uint32_t latched = q4tick::Ld32(r30.u32 + 2640);
    q4tick::St32(r30.u32 + 2644, latched - uint32_t(limit));
  }
  return true;
}

// ---- Engine glue: idCommonLocal::Async catch-up clamp (Common.cpp:2577-2578) -----------------
// idCommonLocal vfunc_07 @0x82175828 (com_timescale == 1 branch):
//   821759c4 bctrl (GetUserCmdMSec) ; 821759c8 stw r3,124(r1) ; 821759cc lwz r9,124(r1) ;
//   821759d0 mulli r8,r9,10 ; 821759e0 add r4,r5,r8 (lastTicMsec + 10*msec) ; 821759e8 cmpw r4,r3
//   82175a14 bctrl ; 82175a1c lwz r4,128(r1) ; 82175a20 mulli r3,r4,10 ; 82175a28 subf r10,r3,r11 ;
//   82175a34 stw r10,lastTicMsec
// Hooks after each mulli: 10*msec -> 10*16 ms (= 20 tics of 8 ms). Only when msec is the fast
// single-player tic (multiplayer keeps 33 ms tics and its original clamp).
void Q4Tick_AsyncCatchupClamp(PPCRegister& product, PPCRegister& msec) {
  if (!q4tick::Fast() || msec.s32 != q4tick::Msec()) return;
  product.s64 = int64_t(msec.s32) * 10 * 16 / q4tick::Msec();
}

// ---- Camera animations (Camera.cpp:1837-1843 Think, :1898-1906 GetViewParms) ----------------
// "if (frameRate == gameLocal.GetMHz()) frame = elapsed / msec (lerp 0) else frame =
// elapsed*frameRate/1000 (with lerp)". mHz (0x837FC44C) is GetUserCmdHz(), set in idGameLocal::
// LoadMap at 8236db7c-8236db90, so at 120 Hz the 60 fps md5camera anims leave the first branch:
// they would play at a true 60 fps (4% longer cinematics) instead of 62.5 frames/s as shipped.
// Restore the 60 Hz path: compare frameRate against 60 and divide by 16.

// idCameraAnim::Think @0x824F4D78:
//   824f4df0 lwz r8,80(r11) (frameRate) ; 824f4df8 lwzx r11,r10,r5 (mHz) ; 824f4dfc cmpw cr6,r8,r11 ;
//   824f4e00 bne 0x824f4e40   (r11 reloaded at 824f4e18 / 824f4e4c-58 on both paths)
// idCameraAnim::GetViewParms @0x824F4FA8:
//   824f5080 lwzx r11,r25,r9 (mHz) ; 824f5084 cmpw cr6,r3,r11 (r3 = frameRate) ; 824f5088 lis r11,...
// Hooks before the cmpw: mHz -> 60 when it is the fast tic rate.
void Q4Tick_CameraAnimMHz(PPCRegister& r11) {
  if (q4tick::Fast() && r11.s32 == q4tick::Hz()) r11.s64 = 60;
}

// Think, then-branch: 824f4e1c subf r6,r8,r11 (elapsed) ; 824f4e20 lwzx r11,r10,r4 (msec) ;
//   824f4e28 divw r10,r6,r11 (frame)  -> hook after: frame = elapsed / 16.
void Q4Tick_CameraAnimThinkFrame(PPCRegister& r6, PPCRegister& r10, PPCRegister& r11) {
  if (!q4tick::Fast() || r11.s32 != q4tick::Msec()) return;
  r10.s64 = int64_t(r6.s32 / 16);
}

// GetViewParms, then-branch: 824f508c lwzx r4,r25,r6 (msec) ; 824f50a4 fmr f31,f30 (lerp = 0) ;
//   824f50b4 subf r10,r5,r7 (elapsed) ; 824f50bc divw r31,r10,r4 (frame) ; 824f50cc b 0x824f5144
// Hook after the divw: frame = elapsed / 16 (identical to 60 Hz on every 16 ms step). The lerp
// (f31, used from 824f55b8 on, same as the 30 fps path) gets the position inside the 16 ms frame so
// the in-between 8 ms tic is interpolated instead of repeating the frame; at elapsed % 16 == 0 it
// is 0 and the view is exactly the 60 Hz one.
void Q4Tick_CameraAnimViewFrame(PPCRegister& r4, PPCRegister& r10, PPCRegister& r31, PPCRegister& f31) {
  if (!q4tick::Fast() || r4.s32 != q4tick::Msec()) return;
  const int elapsed = r10.s32;
  r31.s64 = int64_t(elapsed / 16);
  f31.f64 = elapsed > 0 ? AsFloat(double(elapsed % 16) / 16.0) : 0.0;
}

// ---- rvPlaybackDriver::Start @0x82463050 (Playback.cpp:276) ---------------------------------
// mTransitionTime = numFrames * gameLocal.GetMSec():
//   824631fc lwzx r11,r29,r4 (msec) ; 82463200 mullw r11,r11,r27 (r27 = numFrames) ; 82463204 stw r11,4(r31)
// Hook before the stw: numFrames * 16 (scripts pass 4 -> 64 ms blend, as at 60 Hz).
void Q4Tick_PlaybackTransition(PPCRegister& r11, PPCRegister& r27) {
  if (!q4tick::Fast() || r11.s32 != r27.s32 * q4tick::Msec()) return;
  r11.s64 = int64_t(r27.s32) * 16;
}

// ---- Controller look smoothing (360 joystick path, sub_821C9AA8) -----------------------------
// The stick look step itself is scaled by 60 / GetUserCmdHz() (821c9c38 bctrl GetUserCmdHz ;
// 821c9c94 fdivs f0 = 60/hz ; scales axes and the response-curve range), but the curve output is
// then smoothed once per tic: y = (y + c) * 0.5 (yaw: fRam8390256c, pitch: fRam83902570):
//   821c9d00 lfs f13,16(r29) (0.5) ; 821c9d10 lfs f12,0(r11) (y) ; 821c9d14 fadds f7,f12,f29 ;
//   821c9d18 fmuls f12,f7,f13 ; 821c9d24 stfs f12,0(r11)
//   821c9d38 lfs f0,0(r11) (y) ; 821c9d3c fadds f6,f0,f1 ; 821c9d40 fmuls f0,f6,f13
// At 8 ms tics the filter's time constant halves. Per tic it must be y = k*y + (1-k)*c with
// k = 0.5^(msec/16); feeding f' = 2k*y + (1-2k)*c into the unchanged (f' + c)*0.5 gives exactly
// that. Hooks before the fadds; f12 / f0 are rewritten by the following fmuls.
void Q4Tick_StickSmoothYaw(PPCRegister& y, PPCRegister& c) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::PowFrac(0.5);
  y.f64 = AsFloat(2.0 * k * y.f64 + (1.0 - 2.0 * k) * c.f64);
}
void Q4Tick_StickSmoothPitch(PPCRegister& y, PPCRegister& c) {
  if (!q4tick::Fast()) return;
  const double k = q4tick::PowFrac(0.5);
  y.f64 = AsFloat(2.0 * k * y.f64 + (1.0 - 2.0 * k) * c.f64);
}
