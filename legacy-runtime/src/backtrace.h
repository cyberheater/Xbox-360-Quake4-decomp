#pragma once
#include <cstdint>
#include <string>

#include "ppc_config.h"
#include "ppc_context.h"

// Guest call-stack inspection.
//
// Recompiled functions use the standard PowerPC frame layout, so a backtrace can be rebuilt from
// the guest registers alone: `ctx.lr` is the return address of the function currently running,
// [r1] holds the back-chain and [back_chain - 8] the return address saved by the owning frame.

// Builds the address -> name table (recompiled functions and kernel import thunks).
void BacktraceInit();

// "sub_82C34F20+0x1A4" style description of a guest code address.
std::string SymbolizeGuestAddress(uint32_t addr);

// Prints the guest call stack for the calling thread.
void BacktraceDump(PPCContext& ctx, const char* reason);

// Describes a host code address as the recompiled guest function containing it, e.g.
// "sub_828C3420 (host +0x1A4)". Needs quake4.map next to the executable; falls back to the raw
// module offset when it is missing.
std::string ResolveHostRip(uintptr_t hostRip);

// True if [addr, addr + size) is committed guest memory (so reading it cannot fault).
bool GuestReadable(uint32_t addr, uint32_t size);

// Prints `bytes` bytes at `addr` as big-endian dwords, 8 per line. Skips unreadable memory.
void DumpGuestMemory(const char* label, uint32_t addr, uint32_t bytes);

// Backtrace of any guest thread from its published register context (the thread should be
// blocked or stopped; values are read racily otherwise).
struct GuestThread;
void BacktraceDumpThread(GuestThread& thread, const char* label);

// Compact key of the thread's first `depth` return addresses (for sampling profilers).
std::string BacktraceSampleKey(GuestThread& thread, int depth);

// Backtrace of the *current thread's* last published context, if any. Safe to call from the
// exception handler.
void BacktraceDumpCurrentThread(const char* reason);
