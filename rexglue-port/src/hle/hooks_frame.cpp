// q4hle - frame-level hooks: Swap (present) for the native renderer.
//
// Quake 4 D3D addresses (identified from runtime traces, see docs/hle.md):
//   0x825EAEC0  D3DDevice_Swap(device, frontBufferTexture, flags)

#include <chrono>
#include <cmath>
#include <cstdlib>

#include "hle/capture.h"
#include "hle/log.h"
#include "hle/native.h"
#include "hle/profiler.h"
#include "hle/renderer.h"
#include "hle/trace.h"

namespace {

using Clock = std::chrono::steady_clock;
double g_t_swap = 0, g_t_present = 0, g_t_fence = 0, g_t_kick = 0, g_t_frame = 0;
uint64_t g_n_fence = 0;
Clock::time_point g_last_swap;

double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

template <int Which>
void TimedHook(PPCContext& __restrict ctx, uint8_t* base, q4hle::trace::GuestFn original) {
  const auto t0 = Clock::now();
  original(ctx, base);
  const double d = Ms(t0, Clock::now());
  if (Which == 0) g_t_fence += d, ++g_n_fence;
  else g_t_kick += d;
}

void SwapHook(PPCContext& __restrict ctx, uint8_t* base, q4hle::trace::GuestFn original) {
  const uint32_t front = ctx.r4.u32;
  const auto t0 = Clock::now();
  original(ctx, base);  // keep the guest's frame bookkeeping (fences, frame counters, VdSwap)
  const auto t1 = Clock::now();
  g_t_swap += Ms(t0, t1);
  if (g_last_swap.time_since_epoch().count()) g_t_frame += Ms(g_last_swap, t0);
  g_last_swap = t0;
  q4hle::trace::NoteFrame();
  q4hle::prof::FrameEnd();
  q4hle::capture::FrameEnd(q4hle::trace::FrameCount());
  if (!q4hle::native::Enabled()) return;
  if (!q4hle::native::EnsureDevice()) return;
  // Hands the frame to the render thread (which presents it); waits only if the render thread
  // is more than a frame behind.
  const auto t2 = Clock::now();
  q4hle::render::EndFrame(base, front);
  g_t_present += Ms(t2, Clock::now());
  const uint64_t n = q4hle::trace::FrameCount();
  if (n % 300 == 0) {
    q4hle::Log("timing (ms/frame over 300): frame %.2f swap %.2f submit %.2f fence-wait %.2f (%llu calls) kick %.2f",
               g_t_frame / 300, g_t_swap / 300, g_t_present / 300, g_t_fence / 300,
               static_cast<unsigned long long>(g_n_fence), g_t_kick / 300);
    g_t_frame = g_t_swap = g_t_present = g_t_fence = g_t_kick = 0;
    g_n_fence = 0;
  }
}

struct Register {
  Register() {
    q4hle::trace::SetHandler(0x825EAEC0, SwapHook);
    q4hle::trace::SetHandler(0x825DEB70, TimedHook<0>);  // BlockOnFence
    q4hle::trace::SetHandler(0x825DF160, TimedHook<1>);  // KickOff / make ring space
  }
} g_register;

}  // namespace
