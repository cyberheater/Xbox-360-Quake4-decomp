// q4 - game tic rate (60 or 120 Hz) and helpers for the per-frame timing fixes.
//
// Quake 4 asks the engine for its tic rate (idCommon::GetUserCmdHz / GetUserCmdMSec). Overriding
// them (q4_tick.cpp) runs the game at 120 tics per second with 8 ms of game time per tic. Code that
// is written per game frame with a constant step (instead of scaling by gameLocal.msec) would run
// twice as fast; those places are corrected by mid-function hooks (q4_tick_hooks.toml) whose
// implementations use the helpers below. Every hook must be a no-op at 60 Hz.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>

namespace q4tick {

// Configured tic rate: 60 (default) or 120 (Q4_TICRATE=120).
int Hz();
// Milliseconds of game time per tic (16 or 8).
inline int Msec() { return Hz() == 60 ? 16 : 1000 / Hz(); }
// Guest memory (captured on the first call into an overridden game function).
uint8_t* Base();
inline uint32_t Ld32(uint32_t a);
constexpr uint32_t kGameMsec = 0x837FC448;  // gameLocal.msec
// True when the game is actually running the fast tic: configured faster than 60 Hz AND
// gameLocal.msec is the fast single-player tic. Multiplayer keeps 33 ms tics (q4_tick.cpp only
// overrides the single-player 60/16 values), and there every hook must stay a no-op.
inline bool Fast() { return Hz() != 60 && Base() != nullptr && int(Ld32(kGameMsec)) == Msec(); }
// Fraction of an original 60 Hz frame that one tic represents (1.0 or 0.5).
inline double Frac() { return double(Msec()) / 16.0; }
// Per-frame multiplicative factor (decay, friction, smoothing) converted to one tic.
inline double PowFrac(double factor) { return Fast() ? std::pow(factor, Frac()) : factor; }
// Per-frame blend rate r (x += (target - x) * r) converted to one tic.
inline double RateFrac(double r) { return Fast() ? 1.0 - std::pow(1.0 - r, Frac()) : r; }

inline uint32_t Ld32(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, Base() + a, 4);
  return __builtin_bswap32(v);
}
inline void St32(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(Base() + a, &v, 4);
}
inline float LdF(uint32_t a) {
  const uint32_t v = Ld32(a);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StF(uint32_t a, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  St32(a, v);
}

// Game globals (idGameLocal gameLocal in the 360 executable).
constexpr uint32_t kGameTime = 0x837FC444;  // gameLocal.time (ms)
inline int GameTime() { return int(Ld32(kGameTime)); }
// True on tics that end a 16 ms step of game time (every tic at 60 Hz, every other tic at
// 120 Hz): use it to run "once per original frame" logic at the original rate.
inline bool OnOriginalFrameBoundary() {
  if (!Fast()) return true;
  const int t = GameTime();
  return (t / 16) != ((t - Msec()) / 16);
}

}  // namespace q4tick
