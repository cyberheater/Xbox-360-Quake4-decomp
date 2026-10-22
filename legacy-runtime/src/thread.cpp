#include "thread.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "guest_alloc.h"
#include "imports.h"
#include "kobject.h"
#include "memory.h"

namespace
{
    GuestTlsInfo g_tlsInfo;
    std::atomic<uint32_t> g_nextThreadId{1};
    thread_local GuestThread* t_current = nullptr;
    thread_local PPCContext* t_context = nullptr;

    std::mutex g_mutex;
    bool g_tlsUsed[kMaxTlsSlots] = {};
    std::vector<std::shared_ptr<GuestThread>> g_threads; // keeps every thread alive for the process lifetime

    constexpr SIZE_T kHostStackSize = 16u * 1024 * 1024;

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    // Allocates the guest-visible parts of a thread: stack, static TLS, PCR and a fake KTHREAD.
    bool InitThreadMemory(GuestThread& t, uint32_t stackSize)
    {
        t.stackSize = AlignUp(stackSize ? stackSize : 0x40000, 0x10000);
        t.stackBase = GuestAlloc(GuestDomain::Stack, 0, t.stackSize, 0x10000, true);
        if (!t.stackBase)
        {
            std::fprintf(stderr, "[thread] out of stack address space (%u bytes)\n", t.stackSize);
            return false;
        }

        // Dynamic slots first, static data after them; PCR+0 points at the static part.
        uint32_t tlsTotal = g_tlsInfo.slotCount * 4 + g_tlsInfo.dataSize;
        if (tlsTotal < 0x100)
            tlsTotal = 0x100;

        t.tlsBlock = GuestAlloc(GuestDomain::User, 0, tlsTotal, 0x1000, true);
        if (!t.tlsBlock)
            return false;
        t.tlsStatic = t.tlsBlock + g_tlsInfo.slotCount * 4;

        if (g_tlsInfo.rawDataSize != 0 && g_tlsInfo.rawDataAddress != 0)
            std::memcpy(g_guestBase + t.tlsStatic, g_guestBase + g_tlsInfo.rawDataAddress, g_tlsInfo.rawDataSize);

        t.pcr = KernelAlloc(0x1000);
        t.kthread = KernelAlloc(0x400);

        GuestStore32(t.pcr + 0x00, t.tlsStatic);
        GuestStore32(t.pcr + 0x70, t.stackBase + t.stackSize); // stack base (top)
        GuestStore32(t.pcr + 0x74, t.stackBase);               // stack limit (bottom)
        GuestStore32(t.pcr + 0x100, t.kthread);                // current KTHREAD
        g_guestBase[t.pcr + 0x10C] = 0;                        // current CPU

        GuestStore32(t.kthread + 0x14C, t.id);                 // thread id
        return true;
    }

    void InitContext(PPCContext& ctx, const GuestThread& t)
    {
        ctx.r1.u64 = t.stackBase + t.stackSize - 0x100;
        ctx.r13.u64 = t.pcr;
        ctx.fpscr.loadFromHost();
    }

    void RegisterThreadObject(GuestThread& t, const std::shared_ptr<GuestThread>& self)
    {
        auto obj = std::make_shared<KObject>(KType::Thread);
        obj->thread = self;
        DuplicateHandle(GetCurrentProcess(), t.host, GetCurrentProcess(), &obj->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
        GuestObjectRegister(t.kthread, std::move(obj));
    }

    DWORD WINAPI ThreadMain(LPVOID param)
    {
        auto* t = static_cast<GuestThread*>(param);
        t_current = t;

        PPCContext ctx{};
        InitContext(ctx, *t);
        t_context = &ctx;
        t->regs = &ctx;

        // The context lives on this thread's stack; diagnostics must not see it after we return.
        struct ClearRegs
        {
            GuestThread* thread;
            ~ClearRegs()
            {
                thread->regs = nullptr;
                t_context = nullptr;
            }
        } clearRegs{ t };

        if (t->hostBody)
        {
            t->hostBody(ctx);
            return 0;
        }

        uint32_t target = t->entry;
        if (t->xapiStartup)
        {
            ctx.r3.u64 = t->entry;
            ctx.r4.u64 = t->context;
            target = t->xapiStartup;
        }
        else
        {
            ctx.r3.u64 = t->context;
        }

        if (target < PPC_CODE_BASE || target >= PPC_CODE_BASE + PPC_CODE_SIZE)
        {
            std::fprintf(stderr, "[thread %u] entry 0x%08X is outside the code range\n", t->id, target);
            return 1;
        }

        PPCFunc* fn = PPC_LOOKUP_FUNC(g_guestBase, target);
        if (!fn)
        {
            std::fprintf(stderr, "[thread %u] entry 0x%08X has no recompiled function\n", t->id, target);
            return 1;
        }

        fn(ctx, g_guestBase);

        t->exitCode = ctx.r3.u32;
        return t->exitCode;
    }
}

void SetGuestTlsInfo(const GuestTlsInfo& info)
{
    g_tlsInfo = info;
}

GuestThread* CurrentGuestThread()
{
    return t_current;
}

void ForEachGuestThread(const std::function<void(GuestThread&)>& fn)
{
    std::vector<std::shared_ptr<GuestThread>> copy;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        copy = g_threads;
    }

    for (auto& t : copy)
        fn(*t);
}

PPCContext* CurrentGuestContext()
{
    return t_context;
}

void SetCurrentGuestContext(PPCContext* ctx)
{
    t_context = ctx;
}

std::shared_ptr<GuestThread> AttachMainGuestThread(PPCContext& ctx, uint32_t stackSize)
{
    auto t = std::make_shared<GuestThread>();
    t->id = g_nextThreadId++;

    if (!InitThreadMemory(*t, stackSize))
        return nullptr;

    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &t->host, 0, FALSE, DUPLICATE_SAME_ACCESS);
    t_current = t.get();
    t_context = &ctx;
    t->regs = &ctx;
    InitContext(ctx, *t);

    RegisterThreadObject(*t, t);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threads.push_back(t);
    }
    return t;
}

std::shared_ptr<GuestThread> CreateGuestThread(uint32_t stackSize, uint32_t entry, uint32_t context,
                                               uint32_t xapiStartup, bool suspended)
{
    auto t = std::make_shared<GuestThread>();
    t->id = g_nextThreadId++;
    t->entry = entry;
    t->context = context;
    t->xapiStartup = xapiStartup;
    t->suspendedAtCreate = suspended;

    if (!InitThreadMemory(*t, stackSize))
        return nullptr;

    DWORD hostTid = 0;
    t->host = CreateThread(nullptr, kHostStackSize, ThreadMain, t.get(),
                           CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, &hostTid);
    if (!t->host)
        return nullptr;

    RegisterThreadObject(*t, t);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threads.push_back(t);
    }

    if (!suspended)
        ResumeThread(t->host);
    return t;
}

PPCFunc* LookupGuestFunction(uint32_t guestAddr)
{
    if (guestAddr < PPC_CODE_BASE || guestAddr >= PPC_CODE_BASE + PPC_CODE_SIZE)
        return nullptr;
    return PPC_LOOKUP_FUNC(g_guestBase, guestAddr);
}

std::shared_ptr<GuestThread> CreateGuestWorker(std::function<void(PPCContext&)> body, uint32_t stackSize)
{
    auto t = std::make_shared<GuestThread>();
    t->id = g_nextThreadId++;
    t->hostBody = std::move(body);

    if (!InitThreadMemory(*t, stackSize))
        return nullptr;

    t->host = CreateThread(nullptr, kHostStackSize, ThreadMain, t.get(),
                           CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t->host)
        return nullptr;

    RegisterThreadObject(*t, t);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_threads.push_back(t);
    }

    ResumeThread(t->host);
    return t;
}

uint32_t ResumeGuestThread(GuestThread& t)
{
    const DWORD prev = ResumeThread(t.host);
    return prev == static_cast<DWORD>(-1) ? 0 : prev;
}

uint32_t GuestTlsAlloc()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t i = 0; i < kMaxTlsSlots; i++)
    {
        if (!g_tlsUsed[i])
        {
            g_tlsUsed[i] = true;
            return i;
        }
    }
    return 0xFFFFFFFF;
}

bool GuestTlsFree(uint32_t index)
{
    if (index >= kMaxTlsSlots)
        return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_tlsUsed[index])
        return false;

    g_tlsUsed[index] = false;
    for (auto& t : g_threads)
        t->tlsSlots[index] = 0;
    return true;
}
