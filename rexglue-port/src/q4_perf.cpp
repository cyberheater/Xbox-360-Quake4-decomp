// q4 - CPU-time fixes for the recompiled game (shared by q4 and q4hle).
//
// 1. Frame pacing: idSessionLocal::Frame (0x821B6D80) waits for the async thread's next game tic
//    by polling `while (com_ticNumber < minTic) Sys_Sleep(1);`. A 1 ms host sleep often lasts
//    1.5-2 ms, so at 120 Hz (8.3 ms frames) the game thread regularly wakes up too late and misses
//    the vblank. Sys_Sleep calls from that loop wait for the tic counter itself instead (a short
//    spin with pause/yield, bounded by the requested time), waking within microseconds of the tic.
//
// 2. Native C runtime helpers: the game's own sin, cos and memcpy (PowerPC code, run through the
//    recompiler) are among its hottest functions. They are replaced by the host's versions.
//
// Q4_PERF_NATIVE=0 turns both off (for A/B comparisons).

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("Q4_PERF_NATIVE");
    return !(e && *e == '0');
  }();
  return on;
}

inline uint8_t* HostPtr(uint8_t* base, uint32_t addr) {
#if defined(_WIN32)
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + addr;
#endif
}

// [addr, addr + n) stays inside one region with a single host mapping offset
inline bool SimpleRange(uint32_t addr, uint32_t n) {
  const uint64_t end = uint64_t(addr) + n;
  if (end > 0x100000000ull) return false;
  return (addr >= 0xE0000000u) == ((end - 1) >= 0xE0000000u);
}

constexpr uint32_t kTicNumber = 0x829C1D08;          // com_ticNumber
constexpr uint32_t kSessionFrame = 0x821B6D80;       // idSessionLocal::Frame
constexpr uint32_t kSessionFrameEnd = 0x821B6D80 + 2156;

inline uint32_t LoadBE32(uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, HostPtr(base, addr), 4);
  return __builtin_bswap32(v);
}

inline void CpuRelax() {
#if defined(_M_X64) || defined(__x86_64__)
  _mm_pause();
#else
  std::this_thread::yield();
#endif
}

}  // namespace

// ---- Sys_Sleep (0x82854588: SleepEx(ms, FALSE)) -----------------------------------------------
REX_EXTERN(__imp__sub_82854588);
extern "C" REX_FUNC(sub_82854588) {
  const uint32_t ms = ctx.r3.u32;
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (Enabled() && ms <= 2 && lr >= kSessionFrame && lr < kSessionFrameEnd) {
    // the session's tic wait: return as soon as the async thread has produced a new tic
    using Clock = std::chrono::steady_clock;
    const uint32_t tic0 = LoadBE32(base, kTicNumber);
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms ? ms : 1);
    uint32_t spins = 0;
    while (LoadBE32(base, kTicNumber) == tic0) {
      CpuRelax();
      if (++spins >= 64) {
        spins = 0;
        if (Clock::now() >= deadline) break;
#ifdef _WIN32
        SwitchToThread();
#else
        std::this_thread::yield();
#endif
      }
    }
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_82854588(ctx, base);
}

// ---- sin / cos (0x82841978 / 0x82841A50, double in f1 -> f1) -------------------------------------
REX_EXTERN(__imp__sub_82841978);
extern "C" REX_FUNC(sub_82841978) {
  if (!Enabled()) return __imp__sub_82841978(ctx, base);
  ctx.f1.f64 = std::sin(ctx.f1.f64);
}

REX_EXTERN(__imp__sub_82841A50);
extern "C" REX_FUNC(sub_82841A50) {
  if (!Enabled()) return __imp__sub_82841A50(ctx, base);
  ctx.f1.f64 = std::cos(ctx.f1.f64);
}

// ---- memcpy (0x82844AB0: r3 = dst, r4 = src, r5 = n; returns dst) -------------------------------
REX_EXTERN(__imp__sub_82844AB0);
extern "C" REX_FUNC(sub_82844AB0) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, n = ctx.r5.u32;
  if (Enabled() && n < (256u << 20) && SimpleRange(dst, n ? n : 1) && SimpleRange(src, n ? n : 1)) {
    if (n) std::memmove(HostPtr(base, dst), HostPtr(base, src), n);
    ctx.r3.u64 = dst;
    return;
  }
  __imp__sub_82844AB0(ctx, base);
}
