// xboxkrnl threading and synchronisation exports: threads, TLS, events, timers, waits,
// critical sections, spinlocks and a handful of time/system queries.
#include "import_stubs.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <windows.h>

#include "backtrace.h"
#include "imports.h"
#include "kobject.h"
#include "kstatus.h"
#include "memory.h"
#include "thread.h"

namespace
{
    // Converts a guest timeout pointer (100 ns units, negative = relative) to milliseconds.
    DWORD TimeoutToMs(uint32_t ptr)
    {
        if (!ptr)
            return INFINITE;

        const int64_t v = static_cast<int64_t>(GuestLoad64(ptr));
        if (v == 0)
            return 0;

        if (v < 0)
        {
            const uint64_t ms = (static_cast<uint64_t>(-v) + 9999) / 10000;
            return ms >= INFINITE ? INFINITE - 1 : static_cast<DWORD>(ms);
        }

        // Absolute FILETIME-style time.
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        const int64_t now = (static_cast<int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        if (v <= now)
            return 0;
        return static_cast<DWORD>((v - now) / 10000);
    }

    uint32_t WaitResultToStatus(DWORD r, uint32_t count = 1)
    {
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + count)
            return nt::kSuccess + (r - WAIT_OBJECT_0);
        switch (r)
        {
        case WAIT_TIMEOUT:       return nt::kTimeout;
        case WAIT_IO_COMPLETION: return nt::kUserApc;
        default:                 return nt::kUnsuccessful;
        }
    }

    // Waits like WaitForSingleObjectEx, but reports (once per call) when a wait has been going on
    // for a while: which guest handle, what kind of object, and who is waiting. This is the first
    // thing to look at when the guest "hangs".
    DWORD WaitDiag(PPCContext& ctx, uint32_t guestHandle, const std::shared_ptr<KObject>& obj, DWORD ms, BOOL alertable,
                   const char* api)
    {
        constexpr DWORD kSliceMs = 3000;

        if (GuestThread* t = CurrentGuestThread())
        {
            static thread_local char label[96];
            std::snprintf(label, sizeof(label), "%s(handle 0x%08X, %u ms)", api, guestHandle, ms);
            t->lastImport = label;
        }

        DWORD waited = 0;
        bool reported = false;
        for (;;)
        {
            const DWORD slice = (ms == INFINITE) ? kSliceMs : (ms - waited < kSliceMs ? ms - waited : kSliceMs);
            const DWORD r = WaitForSingleObjectEx(obj->host, slice, alertable);
            if (r != WAIT_TIMEOUT)
                return r;

            waited += slice;
            if (ms != INFINITE && waited >= ms)
                return WAIT_TIMEOUT;

            if (!reported)
            {
                reported = true;
                static const char* kTypeNames[] = { "event", "timer", "thread", "file" };
                GuestThread* t = CurrentGuestThread();
                std::printf("[wait] %s: guest thread %u has waited %u ms on handle 0x%08X (%s%s), timeout %s\n", api,
                            t ? t->id : 0, waited, guestHandle, kTypeNames[static_cast<int>(obj->type)],
                            obj->manualReset ? ", manual-reset" : "", ms == INFINITE ? "infinite" : "finite");
                std::fflush(stdout);
                BacktraceDump(ctx, "long wait");
            }
        }
    }

    bool EventTraceEnabled()
    {
        static const bool on = std::getenv("Q4_EVENTS") != nullptr;
        return on;
    }

    bool IsSignaled(const std::shared_ptr<KObject>& o)
    {
        return o && o->host && WaitForSingleObject(o->host, 0) == WAIT_OBJECT_0;
    }

    // ---- critical sections ------------------------------------------------------------------
    std::mutex g_csMutex;
    std::unordered_map<uint32_t, std::unique_ptr<CRITICAL_SECTION>> g_critSections;

    CRITICAL_SECTION* GetCritSection(uint32_t guestPtr)
    {
        std::lock_guard<std::mutex> lock(g_csMutex);
        auto& slot = g_critSections[guestPtr];
        if (!slot)
        {
            slot = std::make_unique<CRITICAL_SECTION>();
            InitializeCriticalSectionAndSpinCount(slot.get(), 400);
        }
        return slot.get();
    }

    volatile LONG* LockWord(uint32_t guestPtr)
    {
        return reinterpret_cast<volatile LONG*>(g_guestBase + guestPtr);
    }

    void SpinAcquire(uint32_t guestPtr)
    {
        if (InterlockedCompareExchange(LockWord(guestPtr), 1, 0) == 0)
            return;

        GuestThread* t = CurrentGuestThread();
        static thread_local char label[64];
        std::snprintf(label, sizeof(label), "SpinAcquire(0x%08X)", guestPtr);
        if (t)
            t->lastImport = label;

        while (InterlockedCompareExchange(LockWord(guestPtr), 1, 0) != 0)
            YieldProcessor();
    }
}

// ================================================================================================
// Events
// ================================================================================================

// NTSTATUS NtCreateEvent(HANDLE* out, OBJECT_ATTRIBUTES* attrs, ULONG eventType, ULONG initialState)
// eventType: 0 = notification (manual reset), 1 = synchronization (auto reset)
PPC_FUNC(__imp__NtCreateEvent)
{
    const uint32_t outPtr = ctx.r3.u32;
    const bool manual = ctx.r5.u32 == 0;
    const bool initial = ctx.r6.u32 != 0;

    auto obj = std::make_shared<KObject>(KType::Event);
    obj->manualReset = manual;
    obj->host = CreateEventW(nullptr, manual ? TRUE : FALSE, initial ? TRUE : FALSE, nullptr);
    if (!obj->host)
    {
        ctx.r3.u64 = nt::kNoMemory;
        return;
    }

    const uint32_t handle = HandleCreate(std::move(obj));
    if (outPtr)
        GuestStore32(outPtr, handle);

    if (EventTraceEnabled())
    {
        std::printf("[event] NtCreateEvent -> 0x%08X (%s, initial %s)\n", handle, manual ? "manual-reset" : "auto-reset",
                    initial ? "signaled" : "clear");
        BacktraceDump(ctx, "event created");
    }
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtSetEvent(HANDLE h, LONG* prevState)
PPC_FUNC(__imp__NtSetEvent)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Event)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    const uint32_t prevPtr = ctx.r4.u32;
    if (prevPtr)
        GuestStore32(prevPtr, (obj->manualReset && IsSignaled(obj)) ? 1 : 0);

    if (EventTraceEnabled())
    {
        GuestThread* st = CurrentGuestThread();
        std::printf("[event] thread %u sets 0x%08X\n", st ? st->id : 0, ctx.r3.u32);
    }

    SetEvent(obj->host);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtClearEvent(HANDLE h)
PPC_FUNC(__imp__NtClearEvent)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Event)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (EventTraceEnabled())
    {
        GuestThread* t = CurrentGuestThread();
        std::printf("[event] thread %u clears 0x%08X\n", t ? t->id : 0, ctx.r3.u32);
    }

    ResetEvent(obj->host);
    ctx.r3.u64 = nt::kSuccess;
}

// LONG KeSetEvent(KEVENT* e, LONG increment, BOOLEAN wait)
PPC_FUNC(__imp__KeSetEvent)
{
    auto obj = GuestObjectResolve(ctx.r3.u32);
    if (!obj)
    {
        ctx.r3.u64 = 0;
        return;
    }

    if (EventTraceEnabled())
    {
        std::printf("[event] KeSetEvent(guest object 0x%08X)\n", ctx.r3.u32);
        BacktraceDump(ctx, "KeSetEvent");
    }

    const bool wasSignaled = obj->manualReset && IsSignaled(obj);
    SetEvent(obj->host);
    ctx.r3.u64 = wasSignaled ? 1 : 0;
}

// LONG KeResetEvent(KEVENT* e)
PPC_FUNC(__imp__KeResetEvent)
{
    auto obj = GuestObjectResolve(ctx.r3.u32);
    if (!obj)
    {
        ctx.r3.u64 = 0;
        return;
    }

    const bool wasSignaled = IsSignaled(obj) && obj->manualReset;
    ResetEvent(obj->host);
    ctx.r3.u64 = wasSignaled ? 1 : 0;
}

// ================================================================================================
// Timers
// ================================================================================================

// NTSTATUS NtCreateTimer(HANDLE* out, OBJECT_ATTRIBUTES* attrs, ULONG timerType)
// timerType: 0 = notification (manual reset), 1 = synchronization (auto reset)
PPC_FUNC(__imp__NtCreateTimer)
{
    const uint32_t outPtr = ctx.r3.u32;
    const bool manual = ctx.r5.u32 == 0;

    auto obj = std::make_shared<KObject>(KType::Timer);
    obj->manualReset = manual;
    obj->host = CreateWaitableTimerW(nullptr, manual ? TRUE : FALSE, nullptr);
    if (!obj->host)
    {
        ctx.r3.u64 = nt::kNoMemory;
        return;
    }

    const uint32_t handle = HandleCreate(std::move(obj));
    if (outPtr)
        GuestStore32(outPtr, handle);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtSetTimerEx(HANDLE h, LARGE_INTEGER* due, PVOID routine, ULONG mode, PVOID arg,
//                       BOOLEAN resume, LONG periodMs, ULONG unk)
PPC_FUNC(__imp__NtSetTimerEx)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Timer)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    LARGE_INTEGER due;
    due.QuadPart = ctx.r4.u32 ? static_cast<int64_t>(GuestLoad64(ctx.r4.u32)) : 0;

    // APC completion routines are not supported yet.
    SetWaitableTimer(obj->host, &due, static_cast<LONG>(ctx.r9.s32), nullptr, nullptr, FALSE);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtCancelTimer(HANDLE h, BOOLEAN* currentState)
PPC_FUNC(__imp__NtCancelTimer)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Timer)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, IsSignaled(obj) ? 1 : 0);

    CancelWaitableTimer(obj->host);
    ctx.r3.u64 = nt::kSuccess;
}

// ================================================================================================
// Handles and waits
// ================================================================================================

// NTSTATUS NtClose(HANDLE h)
PPC_FUNC(__imp__NtClose)
{
    ctx.r3.u64 = HandleClose(ctx.r3.u32) ? nt::kSuccess : nt::kInvalidHandle;
}

// NTSTATUS NtDuplicateObject(HANDLE h, HANDLE* out, ULONG options)
PPC_FUNC(__imp__NtDuplicateObject)
{
    const uint32_t src = ctx.r3.u32;
    const uint32_t newHandle = HandleDuplicate(src);
    if (!newHandle)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, newHandle);

    constexpr uint32_t kCloseSource = 1;
    if (ctx.r5.u32 & kCloseSource)
        HandleClose(src);

    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtWaitForSingleObjectEx(HANDLE h, ULONG waitMode, BOOLEAN alertable, LARGE_INTEGER* timeout)
PPC_FUNC(__imp__NtWaitForSingleObjectEx)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || !obj->host)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (EventTraceEnabled())
    {
        static std::atomic<unsigned> traced{ 0 };
        if (traced++ < 200000)
        {
            GuestThread* t = CurrentGuestThread();
            std::printf("[event] thread %u waits on 0x%08X%s\n", t ? t->id : 0, ctx.r3.u32,
                        TimeoutToMs(ctx.r6.u32) == INFINITE ? " (infinite)" : "");
        }
    }

    const DWORD r = WaitDiag(ctx, ctx.r3.u32, obj, TimeoutToMs(ctx.r6.u32), ctx.r5.u32 != 0, "NtWaitForSingleObjectEx");
    ctx.r3.u64 = WaitResultToStatus(r);
}

// NTSTATUS NtWaitForMultipleObjectsEx(ULONG count, HANDLE* handles, ULONG waitType, ULONG waitMode,
//                                     BOOLEAN alertable, LARGE_INTEGER* timeout)
// waitType: 0 = wait all, 1 = wait any
PPC_FUNC(__imp__NtWaitForMultipleObjectsEx)
{
    const uint32_t count = ctx.r3.u32;
    const uint32_t handlesPtr = ctx.r4.u32;

    if (count == 0 || count > MAXIMUM_WAIT_OBJECTS || !handlesPtr)
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    HANDLE hosts[MAXIMUM_WAIT_OBJECTS];
    std::shared_ptr<KObject> keepAlive[MAXIMUM_WAIT_OBJECTS];
    for (uint32_t i = 0; i < count; i++)
    {
        keepAlive[i] = HandleGet(GuestLoad32(handlesPtr + i * 4));
        if (!keepAlive[i] || !keepAlive[i]->host)
        {
            ctx.r3.u64 = nt::kInvalidHandle;
            return;
        }
        hosts[i] = keepAlive[i]->host;
    }

    const bool waitAll = ctx.r5.u32 == 0;
    if (GuestThread* t = CurrentGuestThread())
    {
        static thread_local char label[96];
        std::snprintf(label, sizeof(label), "NtWaitForMultipleObjectsEx(%u handles, first 0x%08X, %u ms)", count,
                      count ? GuestLoad32(ctx.r4.u32) : 0, TimeoutToMs(ctx.r8.u32));
        t->lastImport = label;
    }

    const DWORD r = WaitForMultipleObjectsEx(count, hosts, waitAll ? TRUE : FALSE, TimeoutToMs(ctx.r8.u32), ctx.r7.u32 != 0);
    ctx.r3.u64 = WaitResultToStatus(r, count);
}

// NTSTATUS KeWaitForSingleObject(PVOID obj, ULONG reason, ULONG mode, BOOLEAN alertable, LARGE_INTEGER* timeout)
PPC_FUNC(__imp__KeWaitForSingleObject)
{
    auto obj = GuestObjectResolve(ctx.r3.u32);
    if (!obj || !obj->host)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    const DWORD r = WaitDiag(ctx, ctx.r3.u32, obj, TimeoutToMs(ctx.r7.u32), ctx.r6.u32 != 0, "KeWaitForSingleObject");
    ctx.r3.u64 = WaitResultToStatus(r);
}

// NTSTATUS ObReferenceObjectByHandle(HANDLE h, PVOID type, PVOID* out)
PPC_FUNC(__imp__ObReferenceObjectByHandle)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    uint32_t guestObj = 0;
    if (obj->type == KType::Thread && obj->thread)
    {
        guestObj = obj->thread->kthread;
    }
    else
    {
        // Give waitable objects a guest-visible dispatcher header so KeWait* can find them again.
        guestObj = KernelAlloc(0x40);
        g_guestBase[guestObj] = obj->manualReset ? 0 : 1;
        GuestObjectRegister(guestObj, obj);
    }

    if (ctx.r5.u32)
        GuestStore32(ctx.r5.u32, guestObj);
    ctx.r3.u64 = nt::kSuccess;
}

// VOID ObDereferenceObject(PVOID obj)
PPC_FUNC(__imp__ObDereferenceObject)
{
    // Objects live for the process lifetime for now.
    ctx.r3.u64 = 0;
}

// ================================================================================================
// Threads
// ================================================================================================

// NTSTATUS ExCreateThread(HANDLE* out, ULONG stackSize, ULONG* threadId, PVOID xapiStartup,
//                         PVOID startAddress, PVOID startContext, ULONG flags)
PPC_FUNC(__imp__ExCreateThread)
{
    const uint32_t handlePtr = ctx.r3.u32;
    const uint32_t stackSize = ctx.r4.u32;
    const uint32_t idPtr = ctx.r5.u32;
    const uint32_t xapiStartup = ctx.r6.u32;
    const uint32_t startAddress = ctx.r7.u32;
    const uint32_t startContext = ctx.r8.u32;
    const uint32_t flags = ctx.r9.u32;

    const bool suspended = (flags & 1) != 0;

    auto thread = CreateGuestThread(stackSize, startAddress, startContext, xapiStartup, suspended);
    if (!thread)
    {
        ctx.r3.u64 = nt::kNoMemory;
        return;
    }

    auto obj = std::make_shared<KObject>(KType::Thread);
    obj->thread = thread;
    DuplicateHandle(GetCurrentProcess(), thread->host, GetCurrentProcess(), &obj->host, 0, FALSE, DUPLICATE_SAME_ACCESS);

    const uint32_t handle = HandleCreate(std::move(obj));
    if (handlePtr)
        GuestStore32(handlePtr, handle);
    if (idPtr)
        GuestStore32(idPtr, thread->id);

    ctx.r3.u64 = nt::kSuccess;
}

// VOID ExTerminateThread(NTSTATUS exitCode)
PPC_FUNC(__imp__ExTerminateThread)
{
    ExitThread(ctx.r3.u32);
}

// NTSTATUS NtSuspendThread(HANDLE h, ULONG* prevCount)
PPC_FUNC(__imp__NtSuspendThread)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Thread)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    const DWORD prev = SuspendThread(obj->host);
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, prev == static_cast<DWORD>(-1) ? 0 : prev);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtResumeThread(HANDLE h, ULONG* prevCount)
PPC_FUNC(__imp__NtResumeThread)
{
    auto obj = HandleGet(ctx.r3.u32);
    if (!obj || obj->type != KType::Thread)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    const DWORD prev = ResumeThread(obj->host);
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, prev == static_cast<DWORD>(-1) ? 0 : prev);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtYieldExecution()
PPC_FUNC(__imp__NtYieldExecution)
{
    TraceImport("NtYieldExecution", ctx);
    SwitchToThread();
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS KeDelayExecutionThread(ULONG waitMode, BOOLEAN alertable, LARGE_INTEGER* interval)
PPC_FUNC(__imp__KeDelayExecutionThread)
{
    const DWORD ms = TimeoutToMs(ctx.r5.u32);
    TraceImport("KeDelayExecutionThread", ctx);
    SleepEx(ms == INFINITE ? INFINITE : ms, ctx.r4.u32 != 0);
    ctx.r3.u64 = nt::kSuccess;
}

// ULONG KeSetAffinityThread(PVOID thread, ULONG affinity)
PPC_FUNC(__imp__KeSetAffinityThread)
{
    ctx.r3.u64 = 1; // previous affinity
}

// LONG KeSetBasePriorityThread(PVOID thread, LONG increment)
PPC_FUNC(__imp__KeSetBasePriorityThread)
{
    ctx.r3.u64 = 0;
}

// ================================================================================================
// TLS
// ================================================================================================

// ULONG KeTlsAlloc()
PPC_FUNC(__imp__KeTlsAlloc)
{
    ctx.r3.u64 = GuestTlsAlloc();
}

// BOOLEAN KeTlsFree(ULONG index)
PPC_FUNC(__imp__KeTlsFree)
{
    ctx.r3.u64 = GuestTlsFree(ctx.r3.u32) ? 1 : 0;
}

// PVOID KeTlsGetValue(ULONG index)
PPC_FUNC(__imp__KeTlsGetValue)
{
    GuestThread* t = CurrentGuestThread();
    const uint32_t index = ctx.r3.u32;
    ctx.r3.u64 = (t && index < kMaxTlsSlots) ? t->tlsSlots[index] : 0;
}

// BOOLEAN KeTlsSetValue(ULONG index, PVOID value)
PPC_FUNC(__imp__KeTlsSetValue)
{
    GuestThread* t = CurrentGuestThread();
    const uint32_t index = ctx.r3.u32;
    if (!t || index >= kMaxTlsSlots)
    {
        ctx.r3.u64 = 0;
        return;
    }

    t->tlsSlots[index] = ctx.r4.u32;
    ctx.r3.u64 = 1;
}

// ================================================================================================
// Critical sections and spinlocks
// ================================================================================================

PPC_FUNC(__imp__RtlInitializeCriticalSection)
{
    GetCritSection(ctx.r3.u32);
}

PPC_FUNC(__imp__RtlEnterCriticalSection)
{
    CRITICAL_SECTION* cs = GetCritSection(ctx.r3.u32);
    if (TryEnterCriticalSection(cs))
        return;

    // Contended: remember what we block on and report a long wait (a held-forever lock is a hang).
    GuestThread* t = CurrentGuestThread();
    static thread_local char label[64];
    std::snprintf(label, sizeof(label), "RtlEnterCriticalSection(0x%08X)", ctx.r3.u32);
    if (t)
        t->lastImport = label;

    DWORD waited = 0;
    while (!TryEnterCriticalSection(cs))
    {
        Sleep(1);
        if (++waited == 3000)
        {
            std::printf("[crit] thread %u blocked 3 s on critical section 0x%08X (owner host tid %llu)\n",
                        t ? t->id : 0, ctx.r3.u32, static_cast<unsigned long long>(
                            reinterpret_cast<uintptr_t>(cs->OwningThread)));
            BacktraceDumpCurrentThread("crit-wait");
            std::fflush(stdout);
        }
    }
}

PPC_FUNC(__imp__RtlLeaveCriticalSection)
{
    LeaveCriticalSection(GetCritSection(ctx.r3.u32));
}

PPC_FUNC(__imp__RtlTryEnterCriticalSection)
{
    ctx.r3.u64 = TryEnterCriticalSection(GetCritSection(ctx.r3.u32)) ? 1 : 0;
}

// KIRQL KfAcquireSpinLock(PKSPIN_LOCK lock)
PPC_FUNC(__imp__KfAcquireSpinLock)
{
    SpinAcquire(ctx.r3.u32);
    ctx.r3.u64 = 0; // previous IRQL
}

// VOID KfReleaseSpinLock(PKSPIN_LOCK lock, KIRQL oldIrql)
PPC_FUNC(__imp__KfReleaseSpinLock)
{
    InterlockedExchange(LockWord(ctx.r3.u32), 0);
}

PPC_FUNC(__imp__KeAcquireSpinLockAtRaisedIrql)
{
    SpinAcquire(ctx.r3.u32);
}

PPC_FUNC(__imp__KeReleaseSpinLockFromRaisedIrql)
{
    InterlockedExchange(LockWord(ctx.r3.u32), 0);
}

PPC_FUNC(__imp__KfLowerIrql)
{
}

PPC_FUNC(__imp__KeRaiseIrqlToDpcLevel)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__KeEnterCriticalRegion)
{
}

PPC_FUNC(__imp__KeLeaveCriticalRegion)
{
}

PPC_FUNC(__imp__KeLockL2)
{
}

PPC_FUNC(__imp__KeUnlockL2)
{
}

PPC_FUNC(__imp__KeEnableFpuExceptions)
{
}

// ================================================================================================
// System queries
// ================================================================================================

PPC_FUNC(__imp__KeGetCurrentProcessType)
{
    ctx.r3.u64 = 1; // user process
}

// VOID KeQuerySystemTime(LARGE_INTEGER* out)
PPC_FUNC(__imp__KeQuerySystemTime)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    if (ctx.r3.u32)
        GuestStore64(ctx.r3.u32, t);
}

// ULONGLONG KeQueryPerformanceFrequency()
PPC_FUNC(__imp__KeQueryPerformanceFrequency)
{
    ctx.r3.u64 = 50000000; // Xenon time base
}

PPC_FUNC(__imp__KeBugCheck)
{
    std::printf("[kernel] KeBugCheck 0x%08X\n", ctx.r3.u32);
    std::fflush(stdout);
    std::exit(3);
}

PPC_FUNC(__imp__KeBugCheckEx)
{
    std::printf("[kernel] KeBugCheckEx 0x%08X (0x%08X, 0x%08X, 0x%08X, 0x%08X)\n", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                ctx.r6.u32, ctx.r7.u32);
    std::fflush(stdout);
    std::exit(3);
}

PPC_FUNC(__imp__HalReturnToFirmware)
{
    std::printf("[kernel] HalReturnToFirmware(%u)\n", ctx.r3.u32);
    std::fflush(stdout);
    std::exit(0);
}

// Guest `mftb`: the Xenon timebase runs at 50 MHz.
extern "C" uint64_t Q4ReadTimebase()
{
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();

    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    // Split to avoid overflowing 64 bits.
    const uint64_t whole = static_cast<uint64_t>(c.QuadPart / freq);
    const uint64_t rem = static_cast<uint64_t>(c.QuadPart % freq);
    return whole * 50000000ull + rem * 50000000ull / static_cast<uint64_t>(freq);
}
