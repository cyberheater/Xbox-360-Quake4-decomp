// Quake 4 (Xbox 360) static-recompilation host.
//
// Usage: quake4.exe [--run]
//   (no args)  load the XEX image, build the guest function table, print stats.
//   --run      additionally call the guest entry point on the main guest thread.

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <iterator>
#include <thread>
#include <cstdlib>
#include <vector>
#include <windows.h>
#include <psapi.h>

#include "image.h"
#include "xex.h"

#include "backtrace.h"
#include "guest_alloc.h"
#include "import_stubs.h"
#include "imports.h"
#include "memory.h"
#include "ppc_recomp_shared.h"
#include "thread.h"
#include "vfs.h"
#include "watch.h"
#include "xex_headers.h"

#ifndef GAME_DIR
#define GAME_DIR "C:/Quake4/game"
#endif

static bool LoadXex(const char* path, Image& out, GuestTlsInfo& tls, std::vector<uint8_t>& rawBytes)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        std::fprintf(stderr, "[main] cannot open %s\n", path);
        return false;
    }

    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    // Read the TLS header before Xex2LoadImage rewrites anything.
    if (auto* p = static_cast<const uint32_t*>(getOptHeaderPtr(data.data(), XEX_HEADER_TLS_INFO)))
    {
        tls.slotCount = _byteswap_ulong(p[0]);
        tls.rawDataAddress = _byteswap_ulong(p[1]);
        tls.dataSize = _byteswap_ulong(p[2]);
        tls.rawDataSize = _byteswap_ulong(p[3]);
    }

    out = Xex2LoadImage(data.data(), data.size());
    rawBytes = std::move(data);
    return out.data != nullptr;
}

// Fills the indirect-call table that PPC_CALL_INDIRECT_FUNC reads from.
static size_t BuildFunctionTable(uint8_t* base)
{
    size_t count = 0;
    for (size_t i = 0; PPCFuncMappings[i].guest != 0; i++)
    {
        const size_t guest = PPCFuncMappings[i].guest;
        if (guest < PPC_CODE_BASE || guest >= PPC_CODE_BASE + PPC_CODE_SIZE)
            continue;

        PPC_LOOKUP_FUNC(base, guest) = PPCFuncMappings[i].host;
        count++;
    }
    return count;
}

// Reports which guest address a host access violation corresponds to.
static LONG CALLBACK GuestFaultHandler(PEXCEPTION_POINTERS info)
{
    // Write watchpoints trap through here first (protection fault + single-step).
    if (WatchHandleException(info))
        return EXCEPTION_CONTINUE_EXECUTION;

    const auto* rec = info->ExceptionRecord;

    // Integer division by zero is silently "undefined" on PowerPC but traps on x86.
    if (rec->ExceptionCode == EXCEPTION_INT_DIVIDE_BY_ZERO)
    {
        std::fprintf(stderr, "[fault] integer divide by zero in %s\n",
                     ResolveHostRip(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str());
        std::fflush(stderr);
        BacktraceDumpCurrentThread("integer divide by zero");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;

    const uintptr_t addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
    const uintptr_t lo = reinterpret_cast<uintptr_t>(g_guestBase);
    const char* kind = rec->ExceptionInformation[0] == 0 ? "read" : (rec->ExceptionInformation[0] == 1 ? "write" : "exec");
    const auto rip = reinterpret_cast<uintptr_t>(rec->ExceptionAddress) - reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));

    if (g_guestBase && addr >= lo && addr < lo + PPC_MEMORY_SIZE)
    {
        std::fprintf(stderr, "[fault] guest %s of unmapped address 0x%08llX (host rip offset 0x%llX, in %s)\n", kind,
                     static_cast<unsigned long long>(addr - lo), static_cast<unsigned long long>(rip),
                     ResolveHostRip(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str());

        GuestRegionInfo region;
        if (GuestQuery(static_cast<uint32_t>(addr - lo), region))
            std::fprintf(stderr, "[fault]   allocator: state=0x%X allocBase=0x%08X runBase=0x%08X runSize=0x%X\n",
                         region.state, region.allocBase, region.base, region.size);
        std::fflush(stderr);
        BacktraceDumpCurrentThread("guest memory fault");
        return EXCEPTION_CONTINUE_SEARCH;
    }
    else
        std::fprintf(stderr, "[fault] host %s fault at 0x%p (rip offset 0x%llX, in %s); guest base 0x%p, offset from it 0x%llX\n",
                     kind, reinterpret_cast<void*>(addr), static_cast<unsigned long long>(rip),
                     ResolveHostRip(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str(),
                     static_cast<void*>(g_guestBase), static_cast<unsigned long long>(addr - lo));
    std::fflush(stderr);
    BacktraceDumpCurrentThread("host memory fault");
    return EXCEPTION_CONTINUE_SEARCH;
}


// Debug aid: after Q4_WATCHDOG seconds, samples the main thread's instruction pointer a few times
// and prints it as an offset into this module (resolve it against build/quake4.map).
size_t ProbeDump(const char* path);
void DebugDumpVideoState();
void DebugDumpLoadingScreen();

// Q4_POLL=0xADDR[,0xADDR...]: a thread samples these guest words and reports every change with the
// guest call stacks of all threads (a cheap "who wrote this" when page-protection watching misses).
static void StartPoller()
{
    const char* env = std::getenv("Q4_POLL");
    if (!env)
        return;

    std::vector<uint32_t> addrs;
    for (const char* p = env; *p;)
    {
        char* end = nullptr;
        const uint32_t a = static_cast<uint32_t>(std::strtoul(p, &end, 16));
        if (end == p)
            break;
        addrs.push_back(a);
        p = *end == ',' ? end + 1 : end;
    }

    std::thread([addrs]()
    {
        std::vector<uint32_t> last(addrs.size());
        for (size_t i = 0; i < addrs.size(); i++)
            last[i] = GuestLoad32(addrs[i]);

        unsigned reports = 0;
        for (;;)
        {
            for (size_t i = 0; i < addrs.size(); i++)
            {
                const uint32_t v = GuestLoad32(addrs[i]);
                if (v != last[i] && reports < 30)
                {
                    reports++;
                    std::printf("[poll] 0x%08X: 0x%08X -> 0x%08X\n", addrs[i], last[i], v);
                    ForEachGuestThread([](GuestThread& t)
                    {
                        if (t.regs)
                            BacktraceDumpThread(t, "poll");
                    });
                    std::fflush(stdout);
                }
                last[i] = v;
            }
            Sleep(0);
        }
    }).detach();
}

static void StartWatchdog(HANDLE mainThread)
{
    const char* env = std::getenv("Q4_WATCHDOG");
    if (!env)
        return;

    const DWORD seconds = static_cast<DWORD>(std::atoi(env));
    HANDLE dup = nullptr;
    DuplicateHandle(GetCurrentProcess(), mainThread, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);

    std::thread([dup, seconds]()
    {
        Sleep(seconds * 1000);

        // Q4_PROBE=<path>: dump the first-call order of recompiled functions.
        if (const char* probePath = std::getenv("Q4_PROBE"))
        {
            const size_t n = ProbeDump(probePath);
            Sleep(5000);
            std::string second = std::string(probePath) + ".2";
            ProbeDump(second.c_str());
            std::printf("[probe] %zu functions first-called, written to %s\n", n, probePath);
            std::fflush(stdout);
        }

        if (std::getenv("Q4_MEMSCAN"))
        {
            // Which parts of the GPU-visible physical heap hold data? (1 MB granularity, 4 KB pages)
            for (uint32_t mb = 0; mb < 512; mb += 1)
            {
                const uint32_t a = 0xA0000000u + mb * 0x100000u;
                if (!GuestReadable(a, 0x100000))
                    continue;
                uint32_t nz = 0;
                for (uint32_t pg = 0; pg < 256; pg++)
                {
                    const uint64_t* q = reinterpret_cast<const uint64_t*>(g_guestBase + a + pg * 4096);
                    for (int i = 0; i < 512; i++)
                        if (q[i]) { nz++; break; }
                }
                std::printf("[memscan] phys %3u MB: %3u/256 pages non-zero\n", mb, nz);
            }
            std::fflush(stdout);
        }

        if (std::getenv("Q4_COMMONVT"))
        {
            const uint32_t obj = GuestLoad32(0x829A5D90);
            const uint32_t vt = GuestLoad32(obj);
            std::printf("[common] object=0x%08X vtable=0x%08X\n", obj, vt);
            for (uint32_t off = 0; off < 0x100; off += 4)
                std::printf("[common]  vt+%03u = 0x%08X\n", off, GuestLoad32(vt + off));
            std::fflush(stdout);
        }

        // Q4_PROFILE=1: where does the main thread spend its time (host rip -> guest function)?
        if (std::getenv("Q4_PROFILE"))
        {
            std::map<std::string, int> hist;
            const auto moduleBase2 = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
            MODULEINFO mi2{};
            GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi2, sizeof(mi2));
            for (int i = 0; i < 700; i++)
            {
                SuspendThread(dup);
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                const bool ok = GetThreadContext(dup, &c) != 0;
                ResumeThread(dup);
                if (ok)
                {
                    if (c.Rip >= moduleBase2 && c.Rip < moduleBase2 + mi2.SizeOfImage)
                        hist[ResolveHostRip(static_cast<uintptr_t>(c.Rip))]++;
                    else
                        hist["(os)"]++;
                }
                Sleep(2);
            }
            std::vector<std::pair<int, std::string>> v;
            for (auto& kv : hist)
                v.push_back({ kv.second, kv.first });
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 25; i++)
                std::printf("[profile] %5d %s\n", v[i].first, v[i].second.c_str());
            std::fflush(stdout);
        }

        // Q4_SAMPLE=1: histogram of guest call stacks per thread over a few seconds.
        if (std::getenv("Q4_SAMPLE"))
        {
            std::map<std::string, int> hist[64];
            for (int i = 0; i < 300; i++)
            {
                ForEachGuestThread([&](GuestThread& t)
                {
                    if (t.id < 64)
                        {
                        std::string key;
                        if (t.host && t.regs)
                        {
                            CONTEXT c{};
                            c.ContextFlags = CONTEXT_CONTROL;
                            SuspendThread(t.host);
                            const bool ok = GetThreadContext(t.host, &c) != 0;
                            ResumeThread(t.host);
                            if (ok)
                            {
                                HMODULE mod = nullptr;
                                char modName[MAX_PATH] = {};
                                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                       reinterpret_cast<LPCSTR>(c.Rip), &mod) &&
                                    mod != GetModuleHandleA(nullptr))
                                {
                                    GetModuleFileNameA(mod, modName, MAX_PATH);
                                    const char* base = std::strrchr(modName, 92);
                                    char buf[160];
                                    std::snprintf(buf, sizeof(buf), "%s+0x%llX | ", base ? base + 1 : modName,
                                                  static_cast<unsigned long long>(c.Rip - reinterpret_cast<uintptr_t>(mod)));
                                    key = buf;
                                }
                                else
                                {
                                    key = ResolveHostRip(static_cast<uintptr_t>(c.Rip)) + " | ";
                                }
                            }
                        }
                        hist[t.id][key + BacktraceSampleKey(t, t.id == 1 ? 40 : 16)]++;
                    }
                });
                Sleep(20);
            }
            for (int id = 1; id < 64; id++)
            {
                if (hist[id].size() == 1 && hist[id].begin()->first.empty())
                    continue;
                std::vector<std::pair<int, std::string>> v;
                for (auto& kv : hist[id])
                    v.push_back({ kv.second, kv.first });
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < (id == 1 ? 40u : 4u); i++)
                    std::printf("[sample] thread %d  %3d  %s\n", id, v[i].first, v[i].second.c_str());
            }
            std::fflush(stdout);
        }


        // Where is every guest thread? Blocked threads show the guest call that is waiting.
        const auto moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr));
        MODULEINFO mi{};
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(nullptr), &mi, sizeof(mi));

        SuspendThread(dup);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(dup, &c))
        {
            const bool inModule = c.Rip >= moduleBase && c.Rip < moduleBase + mi.SizeOfImage;
            if (inModule)
                std::printf("[watchdog] main thread is running guest code, host rip offset 0x%llX\n",
                            static_cast<unsigned long long>(c.Rip - moduleBase));
            else
                std::printf("[watchdog] main thread is blocked in an OS call (rip 0x%llX)\n",
                            static_cast<unsigned long long>(c.Rip));
        }
        ResumeThread(dup);

        ForEachGuestThread([](GuestThread& t)
        {
            std::printf("[watchdog] thread %u last import: %s\n", t.id, t.lastImport ? t.lastImport : "(none)");
            BacktraceDumpThread(t, "watchdog");
        });

        DebugDumpVideoState();
        DebugDumpLoadingScreen();
        std::fflush(stdout);
    }).detach();
}

int main(int argc, char** argv)
{
    {
        typedef LONG (NTAPI* SetRes)(ULONG, BOOLEAN, PULONG);
        if (auto f = reinterpret_cast<SetRes>(GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution")))
        {
            ULONG cur = 0;
            f(5000, TRUE, &cur); // 0.5 ms timer resolution: guest waits/sleeps are otherwise 15 ms granular
        }
    }
    const bool run = argc > 1 && std::strcmp(argv[1], "--run") == 0;
    AddVectoredExceptionHandler(1, GuestFaultHandler);

    Image image;
    GuestTlsInfo tls;
    std::vector<uint8_t> xexBytes;
    if (!LoadXex(GAME_DIR "/default.xex", image, tls, xexBytes))
        return 1;
    XexHeadersInit(std::move(xexBytes));

    std::printf("[main] image base=0x%zX size=0x%X entry=0x%zX\n", image.base, image.size, image.entry_point);
    std::printf("[main] tls: slots=%u raw=0x%08X dataSize=0x%X rawSize=0x%X\n", tls.slotCount, tls.rawDataAddress,
                tls.dataSize, tls.rawDataSize);

    if (image.base != PPC_IMAGE_BASE || image.size != PPC_IMAGE_SIZE)
    {
        std::fprintf(stderr, "[main] image does not match the recompiled config (0x%llX/0x%llX expected)\n",
                     static_cast<unsigned long long>(PPC_IMAGE_BASE), static_cast<unsigned long long>(PPC_IMAGE_SIZE));
        return 1;
    }

    uint8_t* base = GuestMemoryInit();
    if (!base)
        return 1;

    WatchInitFromEnv();

    // Image + the function-pointer table that follows it, and the kernel data area.
    const size_t tableBytes = PPC_CODE_SIZE * 2;
    if (!GuestCommit(base, PPC_IMAGE_BASE, PPC_IMAGE_SIZE + tableBytes) ||
        !GuestCommit(base, guest::kKernelBase, guest::kKernelSize) ||
        !GuestCommit(base, guest::kGpuRegBase, guest::kGpuRegSize))
        return 1;

    std::memcpy(base + image.base, image.data.get(), image.size);

    VfsInit(L"" GAME_DIR, L"C:/Quake4/save");

    const ImportFixupStats imports = FixupImports(base);
    std::printf("[main] import slots patched: %u functions, %u variables\n", imports.functions, imports.variables);

    const size_t mapped = BuildFunctionTable(base);
    std::printf("[main] guest functions mapped: %zu, import stubs: %u\n", mapped, kImportCount);
    BacktraceInit();

    if (!run)
        return 0;

    SetGuestTlsInfo(tls);

    PPCContext ctx{};
    auto mainThread = AttachMainGuestThread(ctx, 0x100000);
    if (!mainThread)
    {
        std::fprintf(stderr, "[main] failed to create the main guest thread\n");
        return 1;
    }

    std::printf("[main] main thread: id=%u stack=0x%08X..0x%08X pcr=0x%08X tls=0x%08X\n", mainThread->id,
                mainThread->stackBase, mainThread->stackBase + mainThread->stackSize, mainThread->pcr, mainThread->tlsStatic);
    StartPoller();
    StartWatchdog(GetCurrentThread());
    std::printf("[main] calling guest entry 0x%zX\n", image.entry_point);
    std::fflush(stdout);

    auto* entry = PPC_LOOKUP_FUNC(base, image.entry_point);
    if (!entry)
    {
        std::fprintf(stderr, "[main] entry point has no mapped function\n");
        return 1;
    }

    entry(ctx, base);

    std::printf("[main] guest returned, r3=0x%llX, unimplemented import calls=%u\n",
                static_cast<unsigned long long>(ctx.r3.u64), UnimplementedImportCalls());
    return 0;
}
