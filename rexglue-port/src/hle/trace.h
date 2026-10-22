// q4hle - wrappers around Quake 4's Xbox D3D library: call tracing, a hang watchdog, and the
// hook point the native renderer uses to replace individual D3D functions.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace q4hle::trace {

using GuestFn = void (*)(PPCContext& __restrict ctx, uint8_t* base);
// A native handler receives the original recompiled function and decides whether to call it.
using Handler = void (*)(PPCContext& __restrict ctx, uint8_t* base, GuestFn original);

extern const unsigned kNumFuncs;
extern const unsigned kFuncAddr[];
extern const char* const kFuncName[];
extern Handler g_handlers[1024];

// Installs a handler for the D3D function at guest address `addr`. Returns false if the address
// is not one of the wrapped functions. Call during static initialisation or before the guest runs.
bool SetHandler(uint32_t addr, Handler h);
int IndexOf(uint32_t addr);

// Optional check run on the outermost D3D call of each thread (rb_thread.cpp: D3D calls from the
// game thread while the back-end thread is drawing).
extern void (*g_enter_check)(unsigned idx, uint32_t lr);
// ...and when that outermost call returns.
extern void (*g_leave_check)(unsigned idx);

// Called by every wrapper. Cheap when tracing is disabled.
void Enter(unsigned idx, const PPCContext& ctx);
void Leave(unsigned idx, const PPCContext& ctx);

// Frames counted by the swap hook (native mode) or the trace frame boundary.
uint64_t FrameCount();
void NoteFrame();

}  // namespace q4hle::trace
