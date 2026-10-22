#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <windows.h>

#include "ppc_config.h"
#include "ppc_context.h"

// XEX_HEADER_TLS_INFO contents.
struct GuestTlsInfo
{
    uint32_t slotCount = 0;
    uint32_t rawDataAddress = 0;
    uint32_t dataSize = 0;
    uint32_t rawDataSize = 0;
};

constexpr uint32_t kMaxTlsSlots = 64;

struct GuestThread
{
    uint32_t id = 0;
    HANDLE host = nullptr;

    uint32_t stackBase = 0;     // lowest address
    uint32_t stackSize = 0;
    uint32_t pcr = 0;           // r13
    uint32_t kthread = 0;       // fake KTHREAD guest object
    uint32_t tlsBlock = 0;      // allocation backing static TLS
    uint32_t tlsStatic = 0;     // pointer stored at PCR+0

    uint32_t entry = 0;
    uint32_t context = 0;
    uint32_t xapiStartup = 0;   // optional trampoline: xapiStartup(entry, context)
    bool suspendedAtCreate = false;
    std::function<void(PPCContext&)> hostBody; // when set, runs instead of guest code
    uint32_t exitCode = 0;

    uint32_t tlsSlots[kMaxTlsSlots] = {};

    const char* volatile lastImport = nullptr; // most recent traced import (diagnostics)
    PPCContext* regs = nullptr; // register context while the thread runs (for diagnostics)
};

void SetGuestTlsInfo(const GuestTlsInfo& info);

// Creates the thread bookkeeping for the calling (main) host thread and fills `ctx`.
std::shared_ptr<GuestThread> AttachMainGuestThread(PPCContext& ctx, uint32_t stackSize);

// Creates a guest thread. Returns null on failure. `suspended` threads wait for ResumeGuestThread.
std::shared_ptr<GuestThread> CreateGuestThread(uint32_t stackSize, uint32_t entry, uint32_t context,
                                               uint32_t xapiStartup, bool suspended);

// Creates a guest thread (own stack/PCR/TLS) that runs `body` on the host with a ready context.
// Used for interrupt-style workers such as the vblank callback.
std::shared_ptr<GuestThread> CreateGuestWorker(std::function<void(PPCContext&)> body, uint32_t stackSize = 0x10000);

// Returns the recompiled function for a guest address, or null.
PPCFunc* LookupGuestFunction(uint32_t guestAddr);

uint32_t ResumeGuestThread(GuestThread& t);

GuestThread* CurrentGuestThread();

// Calls fn for every guest thread created so far.
void ForEachGuestThread(const std::function<void(GuestThread&)>& fn);

// The register context the calling host thread is executing guest code with (null if none).
// Published so diagnostics such as the fault handler can build a guest backtrace.
PPCContext* CurrentGuestContext();
void SetCurrentGuestContext(PPCContext* ctx);

// KeTls*
uint32_t GuestTlsAlloc();
bool GuestTlsFree(uint32_t index);
