#include "backtrace.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
#include <vector>
#include <windows.h>

#include "memory.h"
#include "ppc_recomp_shared.h"
#include "thread.h"
#include "thread.h"
#include "xex.h"

namespace
{
    struct Symbol
    {
        uint32_t addr;
        std::string name;
    };

    std::vector<Symbol> g_symbols; // sorted by address
    std::mutex g_printMutex;       // keeps multi-line dumps from interleaving

    constexpr int kMaxFrames = 48;

    bool InCode(uint32_t addr)
    {
        return addr >= PPC_CODE_BASE && addr < PPC_CODE_BASE + PPC_CODE_SIZE;
    }
}

void BacktraceInit()
{
    g_symbols.clear();

    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "sub_%08zX", PPCFuncMappings[i].guest);
        g_symbols.push_back({ static_cast<uint32_t>(PPCFuncMappings[i].guest), name });
    }

    // Import thunks have friendlier names.
    for (const auto& r : g_Xex2ImportRecords)
    {
        if (r.type == 1 && !r.name.empty())
        {
            std::string n = r.name;
            if (n.rfind("__imp__", 0) == 0)
                n.erase(0, 7);
            g_symbols.push_back({ r.slot, "[import] " + n });
        }
    }

    std::stable_sort(g_symbols.begin(), g_symbols.end(),
                     [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
}

std::string SymbolizeGuestAddress(uint32_t addr)
{
    char buf[96];

    auto it = std::upper_bound(g_symbols.begin(), g_symbols.end(), addr,
                               [](uint32_t a, const Symbol& s) { return a < s.addr; });
    if (it == g_symbols.begin())
    {
        std::snprintf(buf, sizeof(buf), "0x%08X", addr);
        return buf;
    }

    --it;
    const uint32_t off = addr - it->addr;
    if (off > 0x10000)
    {
        std::snprintf(buf, sizeof(buf), "0x%08X", addr);
        return buf;
    }

    std::snprintf(buf, sizeof(buf), "+0x%X", off);
    return it->name + buf;
}

void BacktraceDump(PPCContext& ctx, const char* reason)
{
    std::lock_guard<std::mutex> lock(g_printMutex);

    GuestThread* thread = CurrentGuestThread();
    const uint32_t stackLo = thread ? thread->stackBase : 0;
    const uint32_t stackHi = thread ? thread->stackBase + thread->stackSize : 0xFFFFFFFF;

    std::printf("[backtrace] %s (thread %u)\n", reason ? reason : "", thread ? thread->id : 0);

    uint32_t r1 = ctx.r1.u32;
    uint32_t lr = static_cast<uint32_t>(ctx.lr);

    int frame = 0;
    std::printf("  #%-2d lr   0x%08X  %s\n", frame++, lr, SymbolizeGuestAddress(lr).c_str());

    for (; frame < kMaxFrames; frame++)
    {
        if (r1 < stackLo + 8 || r1 >= stackHi || !GuestReadable(r1, 4))
            break;

        const uint32_t back = GuestLoad32(r1);
        if (back <= r1 || back >= stackHi || back < stackLo + 8 || !GuestReadable(back - 8, 4))
            break;

        const uint32_t ret = GuestLoad32(back - 8);
        if (!InCode(ret))
            break;

        std::printf("  #%-2d      0x%08X  %s\n", frame, ret, SymbolizeGuestAddress(ret).c_str());
        r1 = back;
    }

    std::fflush(stdout);
}


// Reads r1/lr from a thread's live context; the context lives on that thread's host stack, so it
// can vanish while we look at it.
static bool SafeReadRegs(GuestThread& thread, uint32_t& r1, uint32_t& lr)
{
    // Suspending the thread keeps its context (on the host stack) alive while we copy from it.
    if (!thread.host || SuspendThread(thread.host) == static_cast<DWORD>(-1))
        return false;

    bool ok = false;
    if (PPCContext* regs = thread.regs)
    {
        r1 = regs->r1.u32;
        lr = static_cast<uint32_t>(regs->lr);
        ok = true;
    }

    ResumeThread(thread.host);
    return ok;
}

void BacktraceDumpThread(GuestThread& thread, const char* label)
{
    if (!thread.regs)
    {
        std::lock_guard<std::mutex> lock(g_printMutex);
        std::printf("[backtrace] %s (thread %u): no context\n", label ? label : "", thread.id);
        return;
    }

    std::lock_guard<std::mutex> lock(g_printMutex);

    const uint32_t stackLo = thread.stackBase;
    const uint32_t stackHi = thread.stackBase + thread.stackSize;

    std::printf("[backtrace] %s (thread %u)\n", label ? label : "", thread.id);

    uint32_t r1 = 0;
    uint32_t lr = 0;
    if (!SafeReadRegs(thread, r1, lr))
    {
        std::printf("  (context gone)\n");
        return;
    }

    int frame = 0;
    std::printf("  #%-2d lr   0x%08X  %s\n", frame++, lr, SymbolizeGuestAddress(lr).c_str());

    for (; frame < kMaxFrames; frame++)
    {
        if (r1 < stackLo + 8 || r1 >= stackHi)
            break;

        const uint32_t back = GuestLoad32(r1);
        if (back <= r1 || back >= stackHi || back < stackLo + 8)
            break;

        const uint32_t ret = GuestLoad32(back - 8);
        if (!InCode(ret))
            break;

        std::printf("  #%-2d      0x%08X  %s\n", frame, ret, SymbolizeGuestAddress(ret).c_str());
        r1 = back;
    }

    std::fflush(stdout);
}

std::string BacktraceSampleKey(GuestThread& thread, int depth)
{
    if (!thread.regs)
        return "(no context)";

    std::string key;
    const uint32_t stackLo = thread.stackBase;
    const uint32_t stackHi = thread.stackBase + thread.stackSize;
    uint32_t r1 = 0;
    uint32_t lr = 0;
    if (!SafeReadRegs(thread, r1, lr))
        return "(context gone)";

    for (int frame = 0; frame < depth; frame++)
    {
        if (r1 < stackLo + 8 || r1 >= stackHi || !GuestReadable(r1, 4))
            break;
        const uint32_t back = GuestLoad32(r1);
        if (back <= r1 || back >= stackHi || back < stackLo + 8 || !GuestReadable(back - 8, 4))
            break;
        const uint32_t ret = GuestLoad32(back - 8);
        if (!InCode(ret))
            break;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X ", ret);
        key += buf;
        r1 = back;
    }
    return key;
}

namespace
{
    struct HostSymbol
    {
        uint32_t rva;
        std::string name;
    };

    std::vector<HostSymbol> g_hostSymbols;
    std::once_flag g_hostSymbolsOnce;

    // Parses the MSVC-style link map: "0001:00123456  name  0000000140124456 f obj".
    void LoadHostSymbols()
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, path, MAX_PATH);
        std::string mapPath(path);
        const size_t dot = mapPath.find_last_of('.');
        if (dot != std::string::npos)
            mapPath.resize(dot);
        mapPath += ".map";

        FILE* f = std::fopen(mapPath.c_str(), "r");
        if (!f)
            return;

        char line[1024];
        while (std::fgets(line, sizeof(line), f))
        {
            unsigned seg = 0, off = 0;
            char name[512];
            unsigned long long va = 0;
            if (std::sscanf(line, " %x:%x %511s %llx", &seg, &off, name, &va) == 4 && seg == 1 && va >= 0x140000000ull)
                g_hostSymbols.push_back({ static_cast<uint32_t>(va - 0x140000000ull), name });
        }
        std::fclose(f);

        std::sort(g_hostSymbols.begin(), g_hostSymbols.end(),
                  [](const HostSymbol& a, const HostSymbol& b) { return a.rva < b.rva; });
    }
}

std::string ResolveHostRip(uintptr_t hostRip)
{
    const uint32_t rva = static_cast<uint32_t>(hostRip - reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)));
    std::call_once(g_hostSymbolsOnce, LoadHostSymbols);

    char buf[64];
    if (g_hostSymbols.empty())
    {
        std::snprintf(buf, sizeof(buf), "host+0x%X", rva);
        return buf;
    }

    auto it = std::upper_bound(g_hostSymbols.begin(), g_hostSymbols.end(), rva,
                               [](uint32_t r, const HostSymbol& s) { return r < s.rva; });
    if (it == g_hostSymbols.begin())
    {
        std::snprintf(buf, sizeof(buf), "host+0x%X", rva);
        return buf;
    }

    --it;
    std::string name = it->name;
    static const char kPrefix[] = "__imp__";
    if (name.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0)
        name.erase(0, sizeof(kPrefix) - 1);

    std::snprintf(buf, sizeof(buf), " (host +0x%X)", rva - it->rva);
    return name + buf;
}

bool GuestReadable(uint32_t addr, uint32_t size)
{
    if (!g_guestBase || size == 0 || static_cast<uint64_t>(addr) + size > 0x100000000ull)
        return false;

    // The guest space is one host reservation; walk it region by region.
    uint64_t cur = addr;
    const uint64_t end = static_cast<uint64_t>(addr) + size;
    while (cur < end)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(g_guestBase + cur, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
            return false;

        const uint64_t regionEnd = static_cast<uint64_t>(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - g_guestBase);
        cur = regionEnd;
    }
    return true;
}

void DumpGuestMemory(const char* label, uint32_t addr, uint32_t bytes)
{
    if (!GuestReadable(addr, bytes))
    {
        std::printf("  %s @0x%08X: <unreadable>\n", label, addr);
        return;
    }

    std::printf("  %s @0x%08X:\n", label, addr);
    for (uint32_t off = 0; off < bytes; off += 32)
    {
        std::printf("    +%03X:", off);
        for (uint32_t i = 0; i < 32 && off + i < bytes; i += 4)
            std::printf(" %08X", GuestLoad32(addr + off + i));
        std::printf("\n");
    }
}

void BacktraceDumpCurrentThread(const char* reason)
{
    if (PPCContext* ctx = CurrentGuestContext())
    {
        BacktraceDump(*ctx, reason);

        // Register snapshot. Values are as of the last call boundary (the compiler may keep newer
        // ones in host registers), but they are usually enough to see what was being used.
        std::printf("  regs: r1=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X r9=%08X r10=%08X\n", ctx->r1.u32,
                    ctx->r3.u32, ctx->r4.u32, ctx->r5.u32, ctx->r6.u32, ctx->r7.u32, ctx->r8.u32, ctx->r9.u32,
                    ctx->r10.u32);
        std::printf("        r11=%08X r12=%08X r27=%08X r28=%08X r29=%08X r30=%08X r31=%08X ctr=%08X lr=%08X\n",
                    ctx->r11.u32, ctx->r12.u32, ctx->r27.u32, ctx->r28.u32, ctx->r29.u32, ctx->r30.u32, ctx->r31.u32,
                    static_cast<uint32_t>(ctx->ctr.u64), static_cast<uint32_t>(ctx->lr));
        std::fflush(stdout);
    }
}
