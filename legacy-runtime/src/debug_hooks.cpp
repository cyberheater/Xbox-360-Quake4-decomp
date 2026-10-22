// Debug hooks on recompiled guest functions.
//
// Every recompiled function `sub_XXXXXXXX` is emitted as a weak alias of `__imp__sub_XXXXXXXX`
// (the real body). Defining a strong `sub_XXXXXXXX` here replaces it for direct calls and for the
// indirect-call table alike; the hook then forwards to the original body.
//
// Q4_HOOK_BACKTRACE(address, "label") dumps the guest call stack (plus r3..r6) on every entry.

#include <atomic>
#include <utility>
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include <cstdio>
#include <cstring>
#include <windows.h>

#include "backtrace.h"
#include "thread.h"
#include "gpu.h"
#include "memory.h"

#define Q4_HOOK_BACKTRACE(addr, label)                                                                  \
    extern "C" PPC_FUNC(__imp__sub_##addr);                                                             \
    PPC_FUNC(sub_##addr)                                                                                \
    {                                                                                                   \
        std::printf("[hook] %s: r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n", label, ctx.r3.u32,         \
                    ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);                                                \
        BacktraceDump(ctx, label);                                                                      \
        __imp__sub_##addr(ctx, base);                                                                   \
    }


namespace
{
    // The hooks run on hot paths (fence waits are polled thousands of times per second), so avoid
    // the VirtualQuery in GuestReadable() for pointers the title itself dereferences.
    inline bool HookReadable(uint32_t addr, uint32_t size)
    {
        return addr >= 0x10000 && addr + size > addr && addr < 0xFFFF0000u;
    }
}
namespace
{
    // Snapshot of the most recent call into the D3D/XG resource validator, taken at entry so that
    // what the validator later complains about can be shown as it was handed over.
    struct ArgSnapshot
    {
        uint32_t value = 0;
        bool hasData = false;
        uint32_t data[16] = {};
    };

    struct CallSnapshot
    {
        bool valid = false;
        ArgSnapshot args[8];            // r3..r10

        bool hasTable = false;          // [r3 + 12] -> table of 28-byte entries
        uint32_t tableBase = 0;
        uint32_t table[7 * 40] = {};    // first 40 entries
    };

    // One slot per hooked function; each keeps the most recent call made on this thread.
    enum Slot { kSlotValidator, kSlotBuilder, kSlotDispatcher, kSlotCount };
    thread_local CallSnapshot t_lastCall[kSlotCount];

    const char* const kSlotNames[kSlotCount] = {
        "sub_828C3420 (validator)", "sub_828C7148", "sub_828C73B8",
    };

    uint32_t* const kNoRegs = nullptr;

    void Capture(CallSnapshot& s, PPCContext& ctx)
    {
        const uint32_t regs[8] = { ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                   ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32 };

        s.valid = true;
        for (int i = 0; i < 8; i++)
        {
            ArgSnapshot& a = s.args[i];
            a.value = regs[i];
            a.hasData = regs[i] >= 0x10000 && regs[i] < 0xC0000000 && (regs[i] & 3) == 0 && HookReadable(regs[i], 64);
            if (a.hasData)
                for (int j = 0; j < 16; j++)
                    a.data[j] = GuestLoad32(regs[i] + j * 4);
        }

        s.hasTable = false;
        if (s.args[0].hasData)
        {
            s.tableBase = s.args[0].data[3]; // [r3 + 12]
            if (s.tableBase >= 0x10000 && HookReadable(s.tableBase, sizeof(s.table)))
            {
                s.hasTable = true;
                for (size_t j = 0; j < sizeof(s.table) / 4; j++)
                    s.table[j] = GuestLoad32(s.tableBase + static_cast<uint32_t>(j) * 4);
            }
        }
    }

    void PrintSnapshot(const char* title, const CallSnapshot& s, bool withTable)
    {
        static const char* names[8] = { "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10" };

        std::printf("[desc] most recent %s call (arguments as passed):\n", title);
        for (int i = 0; i < 8; i++)
        {
            const ArgSnapshot& a = s.args[i];
            std::printf("  %-3s = 0x%08X%s\n", names[i], a.value, a.hasData ? "  (points to readable memory)" : "");
            if (!a.hasData)
                continue;

            for (int row = 0; row < 16; row += 8)
            {
                std::printf("      +%02X:", row * 4);
                for (int j = 0; j < 8; j++)
                    std::printf(" %08X", a.data[row + j]);
                std::printf("\n");
            }
        }

        if (withTable && s.hasTable)
        {
            std::printf("  r3's table @0x%08X ([r3+12]), non-empty 28-byte entries (type = (flags>>3)&7, 3 expected):\n",
                        s.tableBase);
            for (int e = 0; e < 40; e++)
            {
                const uint32_t* ent = &s.table[e * 7];
                bool empty = true;
                for (int j = 0; j < 7; j++)
                    empty = empty && ent[j] == 0;
                if (empty)
                    continue;

                std::printf("      [%2d]: %08X %08X %08X %08X %08X %08X %08X   type=%u\n", e, ent[0], ent[1], ent[2], ent[3],
                            ent[4], ent[5], ent[6], (ent[1] >> 3) & 7);
            }
        }
        std::fflush(stdout);
    }
}

// ---- the call chain that leads to the validator ---------------------------------------------------
//   sub_828C73B8 -> sub_828C7148 -> sub_828C3420
extern "C" PPC_FUNC(__imp__sub_828C3420);
extern "C" PPC_FUNC(__imp__sub_828C7148);
extern "C" PPC_FUNC(__imp__sub_828C73B8);

PPC_FUNC(sub_828C3420)
{
    Capture(t_lastCall[kSlotValidator], ctx);
    __imp__sub_828C3420(ctx, base);
}

PPC_FUNC(sub_828C7148)
{
    Capture(t_lastCall[kSlotBuilder], ctx);
    __imp__sub_828C7148(ctx, base);
}

PPC_FUNC(sub_828C73B8)
{
    Capture(t_lastCall[kSlotDispatcher], ctx);
    __imp__sub_828C73B8(ctx, base);
}

// ---- resource slot binding: sub_828C5288(device, slot, ?, resource) ------------------------------
//   Stamps the slot's type field with 7 and only changes it to 3 when sub_828C4978 reports a
//   format code in 20..24 (see the write watchpoint analysis).
extern "C" PPC_FUNC(__imp__sub_828C5288);
extern "C" PPC_FUNC(__imp__sub_828C4978);

namespace
{
    int g_bindCalls = 0;
}

PPC_FUNC(sub_828C5288)
{
    if (g_bindCalls++ < 80)
    {
        std::printf("[bind] #%d sub_828C5288(dev=0x%08X slot=%u r5=0x%08X resource=0x%08X)\n", g_bindCalls, ctx.r3.u32,
                    ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
        std::fflush(stdout);
    }
    __imp__sub_828C5288(ctx, base);
}

PPC_FUNC(sub_828C4978)
{
    const uint32_t obj = ctx.r3.u32;
    const uint32_t record = HookReadable(obj, 4) ? GuestLoad32(obj) : 0;
    __imp__sub_828C4978(ctx, base);

    static int calls = 0;
    if (calls++ < 160)
    {
        std::printf("[format] sub_828C4978(obj=0x%08X, record=0x%08X, format field @0x%08X) -> %u%s\n", obj, record,
                    record + 20, ctx.r3.u32,
                    (ctx.r3.u32 >= 20 && ctx.r3.u32 <= 24) ? "" : "   <-- outside 20..24: slot type stays 7");
        std::fflush(stdout);
    }
}

// ---- loading-screen / intro-movie object ------------------------------------------------------------
// The loading thread (sub_822B37A8) drives the object stored in the global at 0x83086398 through
// its vtable: [12] update, [16] finished?, [20] still playing?, [24] begin fade-out.
void DebugDumpLoadingScreen()
{
    constexpr uint32_t kGlobal = 0x83086398;
    if (!HookReadable(kGlobal, 4))
        return;

    const uint32_t obj = GuestLoad32(kGlobal);
    std::printf("[loadscreen] global 0x%08X -> object 0x%08X\n", kGlobal, obj);
    if (!obj || !HookReadable(obj, 4))
        return;

    const uint32_t vtbl = GuestLoad32(obj);
    std::printf("[loadscreen] vtable 0x%08X\n", vtbl);
    if (!HookReadable(vtbl, 40))
        return;

    for (uint32_t i = 0; i < 10; i++)
    {
        const uint32_t fn = GuestLoad32(vtbl + i * 4);
        std::printf("[loadscreen]   vtbl[%2u] = 0x%08X  %s\n", i * 4, fn, SymbolizeGuestAddress(fn).c_str());
    }
    std::fflush(stdout);
}

// ---- GPU fence wait: sub_825DEB70(device, position, ...) ------------------------------------------
// Waits until the value the GPU publishes at [[device+10384]] reaches `position`. Prints the
// numbers (at most once per second) so a stalled wait can be compared with what the command
// processor has actually written.
extern "C" PPC_FUNC(__imp__sub_825DEB70);
extern "C" PPC_FUNC(__imp__sub_825DEAE8);
extern "C" PPC_FUNC(__imp__sub_825DF160);

PPC_FUNC(sub_825DEB70)
{
    {
        static std::atomic<uint64_t> total{ 0 };
        static std::atomic<uint64_t> lastReport{ 0 };
        const uint64_t n = ++total;
        const uint64_t now = GetTickCount64();
        uint64_t prev = lastReport.load();
        if (now - prev >= 2000 && lastReport.compare_exchange_strong(prev, now))
        {
            std::printf("[fencecalls] total=%llu r3=0x%08X r4=0x%08X r5=0x%08X r6=0x%08X\n",
                        static_cast<unsigned long long>(n), ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
            std::fflush(stdout);
        }
    }
    static std::atomic<uint64_t> lastPrint{ 0 };
    const uint64_t now = GetTickCount64();
    uint64_t prev = lastPrint.load();
    if (now - prev >= 1000 && lastPrint.compare_exchange_strong(prev, now))
    {
        const uint32_t dev = ctx.r3.u32;
        const uint32_t fencePtr = HookReadable(dev + 10384, 4) ? GuestLoad32(dev + 10384) : 0;
        const uint32_t fence = fencePtr && HookReadable(fencePtr, 4) ? GuestLoad32(fencePtr) : 0xDEADDEAD;
        std::printf("[fencewait] dev=0x%08X target(r4)=0x%08X r6=0x%08X fenceAddr=0x%08X *fence=0x%08X softWptr=0x%08X\n", dev,
                    ctx.r4.u32, ctx.r6.u32, fencePtr, fence, HookReadable(dev + 10396, 4) ? GuestLoad32(dev + 10396) : 0);
        std::fflush(stdout);
    }

    // The command processor has executed everything submitted, so a fence the title is waiting
    // for can never advance by itself: complete it (the markers are not all reproduced).
    {
        const uint32_t dev = ctx.r3.u32;
        const uint32_t fencePtr = HookReadable(dev + 10384, 4) ? GuestLoad32(dev + 10384) : 0;
        if (fencePtr && HookReadable(fencePtr, 4))
        {
            // The wait first normalises its target (sub_825DEAE8): the device thread asking for a
            // full flush waits for the latest write position. Do the same to find the real target.
            const uint32_t slot = ctx.r1.u32 - 0x100;
            GuestStore32(slot, ctx.r4.u32);
            PPCContext probe = ctx;
            probe.r3.u64 = dev;
            probe.r4.u64 = slot;
            probe.r5.u64 = ctx.r6.u32;
            __imp__sub_825DEAE8(probe, base);
            const uint32_t target = GuestLoad32(slot);

            // The original wait flushes (kicks) the command buffer first when the target is the
            // latest write position. Skipping that would let the title recycle buffers that are
            // still waiting, unsubmitted, in the ring.
            if (target != 0 && target == GuestLoad32(dev + 10396) && GuestLoad32(dev + 13504) == 0)
            {
                PPCContext flush = ctx;
                flush.r3.u64 = dev;
                __imp__sub_825DF160(flush, base);
            }

            if (target != 0 && static_cast<int32_t>(GuestLoad32(fencePtr) - target) < 0)
            {
                // Let the command processor drain what was submitted (bounded), then complete.
                const uint64_t t0 = GetTickCount64();
                GpuCpWaitDrained(10000);
                    (void)t0;

                if (static_cast<int32_t>(GuestLoad32(fencePtr) - target) < 0)
                {
                    static std::atomic<uint32_t> completed{ 0 };
                    const uint32_t n = ++completed;
                    if (n <= 5 || n % 500 == 0)
                    {
                        std::printf("[fencewait] CP idle: completing fence 0x%08X -> 0x%08X (#%u)\n",
                                    GuestLoad32(fencePtr), target, n);
                        std::fflush(stdout);
                    }
                    GuestStore32(fencePtr, target);
                }
            }
        }
    }

    // The original body only polls the progress marker (and the hang detector); everything it
    // protects has been completed above, so skip it instead of risking an endless spin.
    (void)base;
}

// ---- GPU "reached address" wait: sub_825DDCF8(device, ?, target) -----------------------------------
// Loops until the address the GPU publishes at [[device+10384]+4] reaches `target`.
extern "C" PPC_FUNC(__imp__sub_825DDCF8);

PPC_FUNC(sub_825DDCF8)
{
    static std::atomic<uint32_t> calls{ 0 };
    const uint32_t n = ++calls;

    const uint32_t dev = ctx.r3.u32;
    const uint32_t block = HookReadable(dev + 10384, 4) ? GuestLoad32(dev + 10384) : 0;
    const uint32_t published = block && HookReadable(block + 4, 4) ? GuestLoad32(block + 4) : 0xDEADDEAD;
    const uint32_t target = ctx.r5.u32;
    const bool alreadyOk = ((target - published) & 3) == 0 ||
                           (((target - published) & 3) == 1 && ctx.r4.u32 <= (published & ~3u));

    if (!alreadyOk || n % 200 == 0)
    {
        const GpuCpStats cp = GpuCpGetStats();
        const uint32_t wptrReg = GuestLoad32(guest::kGpuRegBase + 0x1C5 * 4);
        std::printf("[addrwait] #%u dev=0x%08X r4=0x%08X target(r5)=0x%08X published=0x%08X (block 0x%08X) %s\n", n, dev,
                    ctx.r4.u32, target, published, block, alreadyOk ? "ok" : "WILL WAIT");
        std::printf("           CP: wptr reg=0x%X, completed rptr=0x%X, %s, packets=%u memWrites=%u softWptr=0x%X\n", wptrReg,
                    cp.rptr, cp.busy ? "BUSY" : "idle", cp.packets, cp.memoryWrites,
                    HookReadable(dev + 10396, 4) ? GuestLoad32(dev + 10396) : 0);
        std::fflush(stdout);
    }

    // Waiting for GPU progress is pointless when the GPU has executed everything submitted: no
    // further progress marker can ever be written. (Real hardware would have written the marker
    // the title is waiting for; this emulation does not reproduce every marker.)
    static std::atomic<uint32_t> bypassed{ 0 };
    if (!alreadyOk)
    {
        // Give the command processor a moment to catch up, then pretend it has reached the mark.
        const uint64_t t0 = GetTickCount64();
        GpuCpWaitDrained(10000);
            (void)t0;

        const uint32_t b = ++bypassed;
        if (b <= 5 || b % 500 == 0)
        {
            std::printf("[addrwait] command processor idle: not waiting (bypass #%u)\n", b);
            std::fflush(stdout);
        }
        return;
    }

    __imp__sub_825DDCF8(ctx, base);
}

// ---- slot allocator: sub_826904F8(builder, request) -> first slot index ---------------------------
extern "C" PPC_FUNC(__imp__sub_826904F8);

PPC_FUNC(sub_826904F8)
{
    const uint32_t builder = ctx.r3.u32;
    const uint32_t request = ctx.r4.u32;
    __imp__sub_826904F8(ctx, base);

    static int calls = 0;
    if (calls++ < 40)
    {
        std::printf("[alloc] sub_826904F8(builder=0x%08X, request=0x%08X) -> slot %u\n", builder, request, ctx.r3.u32);
        std::fflush(stdout);
    }
}

// ---- the error reporter: sub_828705A8(device, errorCode, fmt, ...) -------------------------------
extern "C" PPC_FUNC(__imp__sub_828705A8);

PPC_FUNC(sub_828705A8)
{
    static int reports = 0;
    if (reports++ < 3)
    {
        std::printf("[hook] engine error reporter: r3=0x%08X code=0x%X r5=0x%08X r6=0x%08X\n", ctx.r3.u32, ctx.r4.u32,
                    ctx.r5.u32, ctx.r6.u32);
        BacktraceDump(ctx, "engine error reporter");

        // Outermost call first, so the argument flow reads top-down.
        for (int slot = kSlotCount - 1; slot >= 0; slot--)
            if (t_lastCall[slot].valid)
                PrintSnapshot(kSlotNames[slot], t_lastCall[slot], slot == kSlotValidator);
    }

    __imp__sub_828705A8(ctx, base);
}

// ---- frame limiter sleep: sub_82854588(ms) ----------------------------------------------------
// The main loop polls a tick counter at 0x829A906C and sleeps until it reaches r29.
extern "C" PPC_FUNC(__imp__sub_82854588);

PPC_FUNC(sub_82854588)
{
    static std::atomic<unsigned> calls{ 0 };
    const unsigned n = calls++;
    if (n == 0 || n == 50 || n == 500 || n == 5000)
    {
        std::printf("[sleep] #%u ticks=%d r29=%d r30=0x%08X r28=%d\n", n,
                    static_cast<int32_t>(GuestLoad32(0x829A906C)), static_cast<int32_t>(ctx.r29.u32), ctx.r30.u32,
                    static_cast<int32_t>(ctx.r28.u32));
        std::fflush(stdout);
    }
    __imp__sub_82854588(ctx, base);
}

// ---- intro bumper movies: sub_821CD7C8(loadingScreen) -----------------------------------------
// The per-frame update of the loading screen finishes once its three bumper movies (id, Raven,
// Activision) have all been marked played (bytes +580..+582). There is no WMV/XMA decoder yet, so
// the movies are marked as played and the loading screen proceeds straight to its fade-out.
extern "C" PPC_FUNC(__imp__sub_821CD7C8);

PPC_FUNC(sub_821CD7C8)
{
    const uint32_t obj = ctx.r3.u32;
    if (obj && HookReadable(obj + 580, 3))
    {
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            std::printf("[movies] skipping intro bumper movies (flags at 0x%08X)\n", obj + 580);
            std::fflush(stdout);
        }
        g_guestBase[obj + 580] = 1;
        g_guestBase[obj + 581] = 1;
        g_guestBase[obj + 582] = 1;
    }

    __imp__sub_821CD7C8(ctx, base);
}

// ---- engine console output: idCommon::Printf / DPrintf / Warning / Error ------------------------
// The retail build discards its console output; show the format strings (and first arguments).
namespace
{
    void LogCommonPrint(const char* kind, PPCContext& ctx)
    {
        static std::atomic<uint32_t> lines{ 0 };
        if (lines++ >= 600)
            return;

        const uint32_t fmt = ctx.r4.u32;
        char text[200] = {};
        if (fmt >= 0x82000000u && fmt < 0xA0000000u)
        {
            for (size_t i = 0; i + 1 < sizeof(text); i++)
            {
                const char c = static_cast<char>(g_guestBase[fmt + i]);
                if (c == 0)
                    break;
                text[i] = (c == '\n' || c == '\r') ? ' ' : c;
            }
        }
        // "%s"-style prints: show the string argument itself.
        char arg[200] = {};
        if (text[0] == '%' && text[1] == 's' && ctx.r5.u32 >= 0x10000)
        {
            for (size_t i = 0; i + 1 < sizeof(arg); i++)
            {
                const char c = static_cast<char>(g_guestBase[ctx.r5.u32 + i]);
                if (c == 0)
                    break;
                arg[i] = (c == 10 || c == 13) ? ' ' : c;
            }
            std::printf("[common] %s: %s\n", kind, arg);
            std::fflush(stdout);
            return;
        }
        char a5[100] = {};
        if (ctx.r5.u32 >= 0x10000 && ctx.r5.u32 < 0x90000000u && std::strstr(kind, "Error"))
        {
            for (size_t i = 0; i + 1 < sizeof(a5); i++)
            {
                const char c = static_cast<char>(g_guestBase[ctx.r5.u32 + i]);
                if (c < 32 || c > 126)
                    break;
                a5[i] = c;
            }
        }
        { GuestThread* ct = CurrentGuestThread(); std::printf("[common-arg] '%s'\n", a5); std::printf("[common] %s: %s | 0x%08X 0x%08X 0x%08X t%u\n", kind, text, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ct ? ct->id : 0); }
        std::fflush(stdout);
    }
}

extern "C" PPC_FUNC(__imp__sub_82175B00);
PPC_FUNC(sub_82175B00)
{
    LogCommonPrint("Printf", ctx);
    if (ctx.r4.u32 >= 0x82000000u && ctx.r4.u32 < 0xA0000000u && std::strncmp(reinterpret_cast<const char*>(g_guestBase + ctx.r4.u32), "%6i vertices", 12) == 0)
        BacktraceDump(ctx, "CM vertices print");
    __imp__sub_82175B00(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82175AC8);
PPC_FUNC(sub_82175AC8)
{
    LogCommonPrint("DPrintf", ctx);
    __imp__sub_82175AC8(ctx, base);
}

extern "C" PPC_FUNC(__imp__sub_82175A98);
PPC_FUNC(sub_82175A98)
{
    LogCommonPrint("Warning/Error", ctx);
    __imp__sub_82175A98(ctx, base);
}

// ---- ring-space wait: sub_825DDDA8(device, position, size) -> end position ------------------------
// Waits until the GPU read pointer has left the region about to be written. The result is the
// end of the region; with no hardware read pointer to follow, hand it back immediately.
extern "C" PPC_FUNC(__imp__sub_825DDDA8);

PPC_FUNC(sub_825DDDA8)
{
    const uint32_t dev = ctx.r3.u32;
    const uint32_t mask = GuestLoad32(dev + 13968);

    // The region about to be written may still be queued for the command processor. Without a
    // hardware read pointer to follow, let it drain completely before the title overwrites it.
    const uint64_t t0 = GetTickCount64();
    GpuCpWaitDrained(10000);
        (void)t0;

    ctx.r3.u64 = (ctx.r4.u32 + ctx.r5.u32) & mask;
    (void)base;
}

// ---- six-slot GPU progress wait: sub_825F53D8() ----------------------------------------------------
extern "C" PPC_FUNC(__imp__sub_825F53D8);

PPC_FUNC(sub_825F53D8)
{
    // Waits until the GPU has worked through the queued work (before buffers are recycled).
    const uint64_t t0 = GetTickCount64();
    GpuCpWaitDrained(10000);
        (void)t0;
    (void)ctx;
    (void)base;
}

// ---- loading-screen start / stop ---------------------------------------------------------------
extern "C" PPC_FUNC(__imp__sub_822B3B70);
PPC_FUNC(sub_822B3B70)
{
    std::printf("[loadscreen] STOP requested (running flag=%u)\n", g_guestBase[0x83086343]);
    BacktraceDump(ctx, "loading screen stop");
    std::fflush(stdout);
    __imp__sub_822B3B70(ctx, base);
    std::printf("[loadscreen] STOP returned\n");
    std::fflush(stdout);
}

extern "C" PPC_FUNC(__imp__sub_822B39C8);
PPC_FUNC(sub_822B39C8)
{
    std::printf("[loadscreen] START requested\n");
    BacktraceDump(ctx, "loading screen start");
    std::fflush(stdout);
    __imp__sub_822B39C8(ctx, base);
}

// ---- session per-frame update: sub_821B61B8(session) --------------------------------------------
// Prints the fields that pick the branch (active gui, map spawned, loading flag, ...) once a second.
extern "C" PPC_FUNC(__imp__sub_821B61B8);
PPC_FUNC(sub_821B61B8)
{
    static std::atomic<uint64_t> last{ 0 };
    const uint64_t now = GetTickCount64();
    uint64_t prev = last.load();
    if (now - prev >= 2000 && last.compare_exchange_strong(prev, now))
    {
        const uint32_t s = ctx.r3.u32;
        auto field = [&](uint32_t off) { return GuestLoad32(s + off); };
        auto deref = [&](uint32_t addr) {
            const uint32_t p = GuestLoad32(addr + 44);
            return p >= 0x10000 ? GuestLoad32(p + 36) : 0xFFFFFFFFu;
        };
        const uint32_t cvarA = deref(0x829A72FC);
        const uint32_t cvarB = deref(0x829A67E8);
        std::printf("[session] this=0x%08X gui=0x%08X map(+8)=%u loading(+2608)=%u noGui(+3376)=%u pending(+3276)=0x%08X "
                    "(+3272)=0x%08X (+3168)=0x%08X flag45=%u cvar1=%u cvar2=%u\n",
                    s, field(2984), field(8), g_guestBase[s + 2608], field(3376), field(3276), field(3272), field(3168),
                    g_guestBase[s + 45], cvarA, cvarB);
        // One-shot dump of the active gui object: words, and strings behind pointer-looking words.
        static bool dumpedGui = false;
        if (!dumpedGui && field(2984) >= 0x10000)
        {
            dumpedGui = true;
            const uint32_t gui = field(2984);
            std::printf("[session] active gui object 0x%08X, vtable 0x%08X\n", gui, GuestLoad32(gui));
            for (uint32_t off = 0; off < 0x180; off += 4)
            {
                const uint32_t w = GuestLoad32(gui + off);
                char str[80] = {};
                if (w >= 0x10000 && w < 0xA0000000u && GuestReadable(w, 8))
                {
                    bool ok = true;
                    size_t n = 0;
                    for (; n < sizeof(str) - 1; n++)
                    {
                        const unsigned char c = g_guestBase[w + n];
                        if (c == 0)
                            break;
                        if (c < 32 || c > 126)
                        {
                            ok = false;
                            break;
                        }
                        str[n] = static_cast<char>(c);
                    }
                    if (!ok || n < 3)
                        str[0] = 0;
                }
                std::printf("[session]   +%03u = 0x%08X  %s\n", off, w, str);
            }
        }
        std::printf("[session] time vars: 7424=%d 7428=%d 7432=%d 7436=%d 7440=%d 7444=%d 7448=%d 7452=%d 7456=%d 7460=%d | "
                    "0x829A906C=%d\n",
                    static_cast<int32_t>(GuestLoad32(0x829C1D00)), static_cast<int32_t>(GuestLoad32(0x829C1D04)),
                    static_cast<int32_t>(GuestLoad32(0x829C1D08)), static_cast<int32_t>(GuestLoad32(0x829C1D0C)),
                    static_cast<int32_t>(GuestLoad32(0x829C1D10)), static_cast<int32_t>(GuestLoad32(0x829C1D14)),
                    static_cast<int32_t>(GuestLoad32(0x829C1D18)), static_cast<int32_t>(GuestLoad32(0x829C1D1C)),
                    static_cast<int32_t>(GuestLoad32(0x829C1D20)), static_cast<int32_t>(GuestLoad32(0x829C1D24)),
                    static_cast<int32_t>(GuestLoad32(0x829A906C)));
        std::fflush(stdout);
    }
    __imp__sub_821B61B8(ctx, base);
}

// ---- session message box: sub_821C1FA8(session, type, titleKey, messageKey, ...) -----------------
extern "C" PPC_FUNC(__imp__sub_821C1FA8);
PPC_FUNC(sub_821C1FA8)
{
    auto str = [](uint32_t a) {
        static thread_local char buf[160];
        buf[0] = 0;
        if (a >= 0x10000 && a < 0xA0000000u && GuestReadable(a, 4))
        {
            size_t n = 0;
            for (; n < sizeof(buf) - 1; n++)
            {
                const char c = static_cast<char>(g_guestBase[a + n]);
                if (c == 0)
                    break;
                buf[n] = c;
            }
            buf[n] = 0;
        }
        return static_cast<const char*>(buf);
    };

    static std::atomic<unsigned> shown{ 0 };
    if (shown++ < 12)
    {
        std::printf("[msgbox] type=%u r5(title)=0x%08X \"%s\"\n", ctx.r4.u32, ctx.r5.u32, str(ctx.r5.u32));
        std::printf("[msgbox]   r6(message)=0x%08X \"%s\" r7=0x%08X r8=0x%08X r9=0x%08X r10=0x%08X\n", ctx.r6.u32,
                    str(ctx.r6.u32), ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
        // The caller (sub_822BDDD4, the Live party state machine) keeps `this` in r30 and its frame in r31.
        const uint32_t self = ctx.r30.u32, frame = ctx.r31.u32;
        if (self >= 0x10000 && frame >= 0x10000)
        {
            std::printf("[msgbox]   live: state[+8]=%u peers[+876]=%d flag[+416]=%u id[r31+344]=0x%08X%08X id[r30+408]=0x%08X%08X\n",
                        GuestLoad32(self + 8), static_cast<int32_t>(GuestLoad32(self + 876)), g_guestBase[self + 416],
                        GuestLoad32(frame + 344), GuestLoad32(frame + 348), GuestLoad32(self + 408), GuestLoad32(self + 412));
        }
        BacktraceDump(ctx, "message box");
        std::fflush(stdout);
    }
    __imp__sub_821C1FA8(ctx, base);
}

// ---- Live party/session creation: sub_822BEA28(live, asHost) ---------------------------------------
extern "C" PPC_FUNC(__imp__sub_822BEA28);
PPC_FUNC(sub_822BEA28)
{
    const uint32_t self = ctx.r3.u32;
    std::printf("[live] sub_822BEA28(this=0x%08X, arg=%u): state[+8]=%u\n", self, ctx.r4.u32 & 0xFF,
                GuestLoad32(self + 8));
    BacktraceDump(ctx, "live create");
    std::fflush(stdout);
    const uint32_t frame = ctx.r1.u32 - 208;
    __imp__sub_822BEA28(ctx, base);
    std::printf("[live]   local xuid slot [frame+144]=0x%08X%08X (frame 0x%08X)\n", GuestLoad32(frame + 144),
                GuestLoad32(frame + 148), frame);
    std::printf("[live]   -> after: state[+8]=%u flag[+416]=%u id[+408]=0x%08X%08X\n", GuestLoad32(self + 8),
                g_guestBase[self + 416], GuestLoad32(self + 408), GuestLoad32(self + 412));
    std::fflush(stdout);
}

// ---- input event queue: sub_822D2548(type, value, value2) ---------------------------------------
extern "C" PPC_FUNC(__imp__sub_822D2548);
PPC_FUNC(sub_822D2548)
{
    static std::atomic<unsigned> n{ 0 };
    if (n++ < 60)
    {
        std::printf("[event] queue type=%u value=%d value2=%d\n", ctx.r4.u32, static_cast<int32_t>(ctx.r5.u32),
                    static_cast<int32_t>(ctx.r6.u32));
        std::fflush(stdout);
    }
    __imp__sub_822D2548(ctx, base);
}

// ---- start-screen controller selection: sub_822B8018(live) -----------------------------------------
extern "C" PPC_FUNC(__imp__sub_822B8018);
PPC_FUNC(sub_822B8018)
{
    static std::atomic<uint64_t> last{ 0 };
    const uint64_t now = GetTickCount64();
    uint64_t prev = last.load();
    if (now - prev >= 3000 && last.compare_exchange_strong(prev, now))
    {
        const uint32_t s = ctx.r3.u32;
        std::printf("[start] live=0x%08X state[+8]=%u controller[+216]=0x%08X f948=%u f950=%u f951=%u f953=%u t944=%u\n", s,
                    GuestLoad32(s + 8), GuestLoad32(s + 216), g_guestBase[s + 948], g_guestBase[s + 950],
                    g_guestBase[s + 951], g_guestBase[s + 953], GuestLoad32(s + 944));
        std::fflush(stdout);
    }
    // The title arms its "press START" detection (this+950) through a virtual call the Live code
    // makes when the front end appears. That call is never reached here, so make it once the main
    // menu has been up for a moment.
    {
        static const uint64_t started = GetTickCount64();
        static bool armed = false;
        const uint32_t s = ctx.r3.u32;
        if (!armed && GetTickCount64() - started > 9000 && g_guestBase[s + 950] == 0)
        {
            armed = true;
            const uint32_t vt = GuestLoad32(s);
            if (PPCFunc* fn = LookupGuestFunction(GuestLoad32(vt + 460)))
            {
                PPCContext c = ctx;
                c.r3.u64 = s;
                std::printf("[start] arming START detection (vtable+460 = 0x%08X)\n", GuestLoad32(vt + 460));
                std::fflush(stdout);
                fn(c, base);
            }
        }
    }

    __imp__sub_822B8018(ctx, base);
}

// ---- controller selection: sub_822D0400(live, pad) ----------------------------------------------
// After a player picks a controller the Live code must hold a (local) party owned by that player;
// otherwise its party watchdog reports "lost connection with your party". The party is created
// through the network session path that is not emulated, so establish the same state directly.
extern "C" PPC_FUNC(__imp__sub_822D0400);
PPC_FUNC(sub_822D0400)
{
    const uint32_t self = ctx.r3.u32;
    __imp__sub_822D0400(ctx, base);

    if (GuestLoad32(self + 216) != 0xFFFFFFFFu)
    {
        for (uint32_t i = 0; i < 8; i++)
        {
            g_guestBase[self + 408 + i] = g_guestBase[self + 208 + i];
            g_guestBase[self + 504 + i] = g_guestBase[self + 208 + i];
        }
        g_guestBase[self + 416] = 1;
        std::printf("[start] controller %u selected; local party id set to 0x%08X%08X\n", GuestLoad32(self + 216),
                    GuestLoad32(self + 408), GuestLoad32(self + 412));
        std::fflush(stdout);
    }
}

extern uint64_t g_q4TotalInserts;
extern uint64_t g_q4TotalLinks;

// ---- title heap: grow request that cannot be satisfied: sub_82363450(heap, size) -----------------
extern "C" PPC_FUNC(__imp__sub_82363450);
PPC_FUNC(sub_82363450)
{
    const uint32_t heap = ctx.r3.u32;
    const uint32_t size = ctx.r4.u32;
    __imp__sub_82363450(ctx, base);
    if (ctx.r3.u32 == 0)
    {
        static int n = 0;
        if (n++ < 6)
        {
            std::printf("[heap] grow FAILED: heap=0x%08X size=0x%X (tree inserts so far: %llu, links: %llu)\n", heap, size,
                        static_cast<unsigned long long>(g_q4TotalInserts), static_cast<unsigned long long>(g_q4TotalLinks));
            for (uint32_t off = 0; off < 0x60; off += 4)
                std::printf("[heap]   +%03u = 0x%08X\n", off, GuestLoad32(heap + off));
            std::printf("[heap]   +8240 = 0x%08X +8244 = 0x%08X +8248 = 0x%08X +8764 = 0x%08X +9196 = %u +9200 = %u +9204 = %u\n",
                        GuestLoad32(heap + 8240), GuestLoad32(heap + 8244), GuestLoad32(heap + 8248),
                        GuestLoad32(heap + 8764), GuestLoad32(heap + 9196), GuestLoad32(heap + 9200),
                        GuestLoad32(heap + 9204));
            
            std::printf("[heap]   pageNodes[8776]=%u pageNodeBase[8772]=0x%08X pages[8788]=%u committedBytes[8796]=0x%X "
                        "limit[8792]=0x%08X base[8800]=0x%08X\n", GuestLoad32(heap + 8776),
                        GuestLoad32(heap + 8772), GuestLoad32(heap + 8788), GuestLoad32(heap + 8796),
                        GuestLoad32(heap + 8792), GuestLoad32(heap + 8800));
            {
                const uint32_t tbl = GuestLoad32(0x83080000u - 16248u);
                std::printf("[heap]   table=0x%08X currentIndex[+156]=%d entries:", tbl, static_cast<int32_t>(GuestLoad32(tbl + 156)));
                for (uint32_t i = 0; i < 12; i++)
                {
                    const uint32_t h = GuestLoad32(tbl + i * 4);
                    if (h)
                        std::printf(" [%u]=0x%08X(base 0x%08X)", i, h, GuestLoad32(h + 8244));
                }
                std::printf("\n");
            }
            unsigned nz = 0;
            for (uint32_t off = 8252; off < 8764; off += 4)
                if (GuestLoad32(heap + off))
                    nz++;
            unsigned nzs = 0;
            for (uint32_t off = 48; off < 8240; off += 4)
                if (GuestLoad32(heap + off))
                    nzs++;
            std::printf("[heap]   non-empty page-run bins=%u, non-empty small bins=%u\n", nz, nzs);
            std::fflush(stdout);
        }
    }
}

// ---- spatial tree insertion: sub_8215BE68(tree, pool, node, link, bounds) -----------------------
// Detects insertions that touch an absurd number of leaves (the symptom of a corrupt bounds value
// or tree) by counting the pool allocations (sub_82158EF0) made under one top-level call.
extern "C" PPC_FUNC(__imp__sub_8215BE68);
uint64_t g_q4TotalInserts = 0;
uint64_t g_q4TotalLinks = 0;
static uint32_t g_treeDepth = 0;
static uint64_t g_treeAllocs = 0;
static uint32_t g_treeBounds = 0, g_treeNode = 0;

PPC_FUNC(sub_8215BE68)
{
    if (g_treeDepth == 0)
    {
        g_q4TotalInserts++;
        g_treeAllocs = 0;
        g_treeBounds = ctx.r7.u32;
        g_treeNode = ctx.r5.u32;
    }
    g_treeDepth++;
    __imp__sub_8215BE68(ctx, base);
    g_treeDepth--;
}

extern "C" PPC_FUNC(__imp__sub_82158EF0);
PPC_FUNC(sub_82158EF0)
{
    if (g_treeDepth > 0)
        g_q4TotalLinks++;
    if (g_treeDepth > 0 && ++g_treeAllocs == 5000)
    {
        auto f = [&](uint32_t off) {
            uint32_t w = GuestLoad32(g_treeBounds + off);
            float v;
            std::memcpy(&v, &w, 4);
            return v;
        };
        std::printf("[tree] one insertion made 5000 pool allocations: node=0x%08X bounds=(%g %g %g)-(%g %g %g)\n",
                    g_treeNode, f(0), f(4), f(8), f(12), f(16), f(20));
        // Dump the first levels of the tree: axis, split distance, children.
        struct Dumper
        {
            static void Node(uint32_t n, int depth)
            {
                if (depth > 4 || n < 0x10000)
                    return;
                const int32_t axis = static_cast<int32_t>(GuestLoad32(n));
                uint32_t w = GuestLoad32(n + 4);
                float dist;
                std::memcpy(&dist, &w, 4);
                std::printf("[tree] %*snode 0x%08X axis=%d dist=%g (raw 0x%08X)\n", depth * 2, "", n, axis, dist, w);
                if (axis != -1)
                {
                    Node(GuestLoad32(n + 20), depth + 1);
                    Node(GuestLoad32(n + 24), depth + 1);
                }
            }
        };
        Dumper::Node(g_treeNode, 0);

        // Whole-tree statistics (guarding against cycles / shared subtrees).
        {
            std::set<uint32_t> seen;
            std::vector<std::pair<uint32_t, int>> stack{ { g_treeNode, 0 } };
            uint64_t leaves = 0, inner = 0, shared = 0;
            int maxDepth = 0;
            while (!stack.empty() && seen.size() < 4000000)
            {
                auto [n, d] = stack.back();
                stack.pop_back();
                if (n < 0x10000)
                    continue;
                if (!seen.insert(n).second)
                {
                    shared++;
                    continue;
                }
                maxDepth = std::max(maxDepth, d);
                if (static_cast<int32_t>(GuestLoad32(n)) == -1)
                {
                    leaves++;
                }
                else
                {
                    inner++;
                    stack.push_back({ GuestLoad32(n + 20), d + 1 });
                    stack.push_back({ GuestLoad32(n + 24), d + 1 });
                }
            }
            std::printf("[tree] stats: %llu inner nodes, %llu leaves, %llu shared references, max depth %d\n", static_cast<unsigned long long>(inner), static_cast<unsigned long long>(leaves),
                        static_cast<unsigned long long>(shared), maxDepth);
        }

        BacktraceDump(ctx, "tree explosion");
        std::fflush(stdout);
    }
    __imp__sub_82158EF0(ctx, base);
}

// ---- physical-memory pool: sub_822D2F80(pool, ?, size) -----------------------------------------
extern "C" PPC_FUNC(__imp__sub_822D2F80);
PPC_FUNC(sub_822D2F80)
{
    static int shown = 0;
    const uint32_t pool = ctx.r3.u32;
    const uint32_t top = GuestLoad32(pool + 44);
    if (shown < 6 || (top != 0 && top < 0x10000000u && shown < 40))
    {
        shown++;
        std::printf("[pool] sub_822D2F80 pool=0x%08X base[+40]=0x%08X top[+44]=0x%08X free[+52]=0x%08X r5=0x%X\n", pool,
                    GuestLoad32(pool + 40), top, GuestLoad32(pool + 52), ctx.r5.u32);
        std::fflush(stdout);
    }
    // Validate every size-class chain of this pool: nodes must be physical-window pointers.
    {
        static int reports = 0;
        for (uint32_t slot = 14; slot < 14 + 48 && reports < 3; slot++)
        {
            uint32_t node = GuestLoad32(pool + slot * 4);
            for (int hop = 0; node != 0 && hop < 64; hop++)
            {
                if (node < 0xA0000000u || node >= 0xC0000000u)
                {
                    reports++;
                    std::printf("[pool] BAD chain: pool=0x%08X slot=%u hop=%d node=0x%08X\n", pool, slot, hop, node);
                    // dump the previous node (head of the chain at hop 0)
                    uint32_t cur = GuestLoad32(pool + slot * 4);
                    for (int k = 0; k < hop && cur >= 0xA0000000u && cur < 0xC0000000u; k++)
                    {
                        std::printf("[pool]   node[%d]=0x%08X: %08X %08X %08X %08X %08X %08X\n", k, cur,
                                    GuestLoad32(cur), GuestLoad32(cur + 4), GuestLoad32(cur + 8), GuestLoad32(cur + 12),
                                    GuestLoad32(cur + 16), GuestLoad32(cur + 20));
                        cur = GuestLoad32(cur);
                    }
                    std::fflush(stdout);
                    break;
                }
                node = GuestLoad32(node);
            }
        }
    }

    __imp__sub_822D2F80(ctx, base);
}

// ---- command-buffer kick: sub_825DE5D8 ---------------------------------------------------------
// Moves the ring-buffer write pointer. The title's buffer-lifetime tracking assumes a GPU that
// keeps up; this one is far slower, so a kick does not return until the processor has consumed what
// was submitted (a synchronous GPU). Set Q4_ASYNC_GPU=1 to disable.
extern "C" PPC_FUNC(__imp__sub_825DE5D8);
PPC_FUNC(sub_825DE5D8)
{
    static std::atomic<unsigned> kicks{ 0 };
    const uint32_t before = GuestLoad32(guest::kGpuRegBase + 0x1C5 * 4);
    __imp__sub_825DE5D8(ctx, base);
    const uint32_t after = GuestLoad32(guest::kGpuRegBase + 0x1C5 * 4);
    if (kicks++ < 400)
    {
        std::printf("[kick] #%u wptr %u -> %u (rptr=%u)\n", kicks.load(), before, after, GpuCpGetStats().rptr);
        std::fflush(stdout);
    }

    static const bool async = std::getenv("Q4_ASYNC_GPU") != nullptr;
    if (!async)
        GpuCpWaitDrained(30000);
}

// ---- other idCommon entry points (Error / FatalError / Warning ...) -------------------------------
#define Q4_COMMON_HOOK(ADDR, NAME)                                  \
    extern "C" PPC_FUNC(__imp__sub_##ADDR);                         \
    PPC_FUNC(sub_##ADDR)                                            \
    {                                                               \
        LogCommonPrint(NAME, ctx);                                  \
        __imp__sub_##ADDR(ctx, base);                               \
    }

Q4_COMMON_HOOK(8216D6F8, "vt+144 (Error?)")
Q4_COMMON_HOOK(8216D628, "vt+148 (FatalError?)")

// ---- map-load progress tracing ----------------------------------------------------------------------
#define Q4_TRACE_CALL(ADDR)                                                         \
    extern "C" PPC_FUNC(__imp__sub_##ADDR);                                         \
    PPC_FUNC(sub_##ADDR)                                                            \
    {                                                                               \
        std::printf("[trace] enter sub_" #ADDR "\n");                               \
        std::fflush(stdout);                                                        \
        __imp__sub_##ADDR(ctx, base);                                               \
        std::printf("[trace] leave sub_" #ADDR " r3=%08X\n", ctx.r3.u32);           \
        std::fflush(stdout);                                                        \
    }

Q4_TRACE_CALL(821B3418)
Q4_TRACE_CALL(821B2AE0)
Q4_TRACE_CALL(821B27E8)
Q4_TRACE_CALL(8236DB10)
Q4_TRACE_CALL(8236F200)
