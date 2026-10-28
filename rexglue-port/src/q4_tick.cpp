// q4 - game tic rate override (see q4_tick.h).
//
//   0x8216EB48  idCommonLocal::GetUserCmdHz    returns 30 (multiplayer) or 60
//   0x8216EBC8  idCommonLocal::GetUserCmdMSec  returns 33 (multiplayer) or 16
//
// Only the single-player values are changed. At 120 Hz the game advances 8 ms per tic and the
// async tic timer fires every 8 ms (125 tics per real second), so game time still advances
// 1000 ms per real second, exactly as at 60 Hz (16 ms x 62.5). Animation, sound and dialogue
// stay in step because they are all driven by game time or real time, not by the frame count.
#include "q4_tick.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

#include <rex/cvar.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

REXCVAR_DEFINE_INT32(q4_tickrate, 0, "Game",
                     "Game logic rate in Hz: 0 = match the display (120 on displays of 120 Hz or more, "
                     "otherwise 60), 60 (original) or 120. Only the native renderer (q4hle) shows more "
                     "than 60 fps, so q4.exe treats 0 as 60. The environment variable Q4_TICRATE "
                     "overrides it");

namespace q4tick {
namespace {
// Refresh rate of the primary display (0 if unknown).
int DisplayHz() {
#ifdef _WIN32
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1)
    return int(dm.dmDisplayFrequency);
#endif
  return 0;
}

int ParseHz() {
  const char* e = std::getenv("Q4_TICRATE");
  const int v = e ? std::atoi(e) : int(REXCVAR_GET(q4_tickrate));
  if (v == 60 || v == 120) return v;
#ifdef Q4_HLE
  // Auto: the timing fixes cover 60 and 120 Hz, so pick the one that suits the display.
  // 119/120/144/165/240 Hz displays get 120; 59.94/60/75/100 Hz displays get 60.
  // The CRT beam simulation (q4hle_crt, GRAPHICS menu) needs several display refreshes per game
  // frame, so it keeps the game at 60.
  const int display = DisplayHz();
  std::string crt = rex::cvar::GetFlagByName("q4hle_crt");
  if (const char* ce = std::getenv("Q4_HLE_CRT"); ce && *ce) crt = ce;
  const bool crt_on = !crt.empty() && std::atoi(crt.c_str()) > 0;
  const int hz = display >= 119 && !crt_on ? 120 : 60;
  std::fprintf(stderr, "q4: display refresh %d Hz -> game rate %d Hz (q4_tickrate = 0, auto%s)\n", display, hz,
               crt_on ? ", CRT simulation on" : "");
  return hz;
#else
  return 60;  // q4.exe presents at 60 Hz
#endif
}
uint8_t* g_base = nullptr;
}  // namespace

int Hz() {
  static const int hz = ParseHz();
  return hz;
}

uint8_t* Base() { return g_base; }
void CaptureBase(uint8_t* base) { g_base = base; }

}  // namespace q4tick

namespace q4hle::tick {
int Hz() { return q4tick::Hz(); }
int RefreshHz() { return q4tick::Hz(); }
}  // namespace q4hle::tick

REX_EXTERN(__imp__sub_8216EB48);
extern "C" REX_FUNC(sub_8216EB48) {
  q4tick::CaptureBase(base);
  __imp__sub_8216EB48(ctx, base);
  const int hz = q4tick::Hz();
  if (hz != 60 && ctx.r3.u32 == 60) ctx.r3.u64 = uint64_t(hz);
}

REX_EXTERN(__imp__sub_8216EBC8);
extern "C" REX_FUNC(sub_8216EBC8) {
  q4tick::CaptureBase(base);
  __imp__sub_8216EBC8(ctx, base);
  const int hz = q4tick::Hz();
  if (hz != 60 && ctx.r3.u32 == 16) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::fprintf(stderr, "q4: game tic rate %d Hz (%d ms per tic)\n", hz, 1000 / hz);
    }
    ctx.r3.u64 = uint64_t(1000 / hz);
  }
}

// ---------------------------------------------------------------------------------------------
// Script frame waits. Map scripts use sys.waitFrame() / $entity.waitFrame() both to yield until the
// next frame (fine at any rate) and as a frame-counted delay or per-frame increment (a boss's
// shield regenerating a fixed amount per frame, fades and turns stepped per frame). At 120 Hz the
// latter would run twice as fast, so a script frame wait lasts 16 ms of game time (two tics), as
// it did at 60 Hz. The script events that report the frame time / rate keep reporting 60 Hz so
// scripts that derive frame counts from them stay consistent with the 16 ms waits.
//
//   0x824747C0  idThread::WaitFrame             waitingUntil = time + msec   (thread at r3)
//   0x823F3F78  idEntity::Event_WaitFrame        same, for the current thread (0x83086600)
//   0x82476B50  thread event that waits a frame after a game call (r3 = thread)
//   0x82476C18  thread event that waits a frame after a game call (r3 = thread)
//   0x82476608  idThread::Event_GetFrameTime     returns MS2SEC(msec)
//   0x82476658  idThread::Event_GetTicsPerSecond returns the game's tic rate
namespace {
constexpr uint32_t kCurrentThread = 0x83086600;
constexpr uint32_t kThreadWaitingUntil = 0xC;
constexpr uint32_t kScriptReturn = 0x837DF584;  // pointer to the interpreter's return value slot

inline uint32_t Ld(uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline void St(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}
using q4tick::kGameTime;
using q4tick::kGameMsec;

// Stretch a one-tic script wait to 16 ms of game time.
void StretchFrameWait(uint8_t* base, uint32_t thread) {
  if (q4tick::Hz() == 60 || !thread) return;
  const uint32_t time = Ld(base, kGameTime), msec = Ld(base, kGameMsec);
  if (msec >= 16) return;
  if (Ld(base, thread + kThreadWaitingUntil) == time + msec) St(base, thread + kThreadWaitingUntil, time + 16);
}

void ReturnFloat(uint8_t* base, float f) {
  const uint32_t slot = Ld(base, Ld(base, kScriptReturn) + 4);
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  St(base, slot, bits);
}
}  // namespace

REX_EXTERN(__imp__sub_824747C0);
extern "C" REX_FUNC(sub_824747C0) {
  const uint32_t thread = ctx.r3.u32;
  __imp__sub_824747C0(ctx, base);
  StretchFrameWait(base, thread);
}

REX_EXTERN(__imp__sub_823F3F78);
extern "C" REX_FUNC(sub_823F3F78) {
  __imp__sub_823F3F78(ctx, base);
  StretchFrameWait(base, Ld(base, kCurrentThread));
}

REX_EXTERN(__imp__sub_82476B50);
extern "C" REX_FUNC(sub_82476B50) {
  const uint32_t thread = ctx.r3.u32;
  __imp__sub_82476B50(ctx, base);
  StretchFrameWait(base, thread);
}

REX_EXTERN(__imp__sub_82476C18);
extern "C" REX_FUNC(sub_82476C18) {
  const uint32_t thread = ctx.r3.u32;
  __imp__sub_82476C18(ctx, base);
  StretchFrameWait(base, thread);
}

REX_EXTERN(__imp__sub_82476608);
extern "C" REX_FUNC(sub_82476608) {
  __imp__sub_82476608(ctx, base);
  // Only on the fast single-player tic (multiplayer keeps its 33 ms / 30 Hz values).
  if (q4tick::Hz() != 60 && Ld(base, kGameMsec) < 16) ReturnFloat(base, 0.016f);
}

REX_EXTERN(__imp__sub_82476658);
extern "C" REX_FUNC(sub_82476658) {
  __imp__sub_82476658(ctx, base);
  if (q4tick::Hz() != 60 && Ld(base, kGameMsec) < 16) ReturnFloat(base, 60.0f);
}

// Pipeline check for the mid-function hooks (q4_tick_hooks.toml): runs inside the recompiled
// idCommonLocal::GetUserCmdMSec, before `stw r3,116(r1)`.
void Q4Tick_Probe(PPCRegister& r3) {
  (void)r3;
  static bool once = false;
  if (!once) {
    once = true;
    std::fprintf(stderr, "q4: tic-rate hooks active (%d Hz)\n", q4tick::Hz());
  }
}
