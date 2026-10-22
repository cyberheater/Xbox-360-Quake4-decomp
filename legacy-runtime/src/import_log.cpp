#include "import_stubs.h"
#include "thread.h"

#include "backtrace.h"
#include <cstring>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_set>

static std::atomic<unsigned> g_calls{0};
static std::mutex g_mutex;
static std::unordered_set<std::string> g_seen;

// Safety valve so a --run against an unfinished runtime cannot spin forever.
static constexpr unsigned kMaxUnimplementedCalls = 2000;

void UnimplementedImport(const char* name, PPCContext& ctx, uint8_t* /*base*/)
{
    const unsigned n = ++g_calls;

    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_seen.insert(name).second)
        {
            first = true;
            std::printf("[import] unimplemented: %-40s r3=0x%08X r4=0x%08X r5=0x%08X (call #%u)\n",
                        name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, n);
            std::fflush(stdout);
        }
    }

    // Q4_BT=1: show where each unimplemented import was first called from.
    static const bool backtraces = std::getenv("Q4_BT") != nullptr;
    if (first && backtraces)
        BacktraceDump(ctx, name);

    if (n >= kMaxUnimplementedCalls)
    {
        std::printf("[import] %u unimplemented calls - aborting run\n", n);
        std::fflush(stdout);
        std::exit(2);
    }

    ctx.r3.u64 = 0;
}

void TraceImport(const char* name, PPCContext& ctx)
{
    if (GuestThread* t = CurrentGuestThread())
        t->lastImport = name;

    // Q4_BT_IMPORT=<substring>: backtrace every call to matching imports (for imports that call
    // TraceImport).
    static const char* btFilter = std::getenv("Q4_BT_IMPORT");
    if (btFilter && std::strstr(name, btFilter))
        BacktraceDump(ctx, name);

    static const bool enabled = std::getenv("Q4_TRACE") != nullptr;
    if (!enabled)
        return;

    GuestThread* cur = CurrentGuestThread();
    std::printf("[call] %s(0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X, 0x%08X) t%u\n", name, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, cur ? cur->id : 0);
    std::fflush(stdout);
}

unsigned UnimplementedImportCalls()
{
    return g_calls.load();
}
