// xboxkrnl memory manager exports: Nt*VirtualMemory and Mm*.
#include "import_stubs.h"

#include <cstdio>
#include <string>
#include <cstdlib>

#include "guest_alloc.h"
#include "kstatus.h"
#include "memory.h"

namespace
{
    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    constexpr uint32_t kPageNoAccess = 0x01;
    constexpr uint32_t kPageReadWrite = 0x04;
    constexpr uint32_t kMemFree = 0x10000;
    constexpr uint32_t kMemPrivate = 0x20000;
}

// NTSTATUS NtAllocateVirtualMemory(PVOID* base, PSIZE_T size, ULONG type, ULONG protect, ULONG dbg)
PPC_FUNC(__imp__NtAllocateVirtualMemory)
{
    const uint32_t basePtr = ctx.r3.u32;
    const uint32_t sizePtr = ctx.r4.u32;
    const uint32_t type = ctx.r5.u32;

    if (!basePtr || !sizePtr)
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    uint32_t addr = GuestLoad32(basePtr);
    const uint32_t size = GuestLoad32(sizePtr);

    const bool large = (type & (mem::kLargePages | mem::k16MbPages | mem::k4MbPages)) != 0;
    const uint32_t pageSize = large ? 0x10000 : 0x1000;

    if (size == 0 || (type & (mem::kCommit | mem::kReserve | mem::kReset)) == 0)
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    uint32_t allocSize = AlignUp(size, pageSize);
    {
        // Experiment: Q4_ARENA_SCALE=<n> enlarges the title's big heap arenas.
        static const uint32_t scale = [] {
            const char* e = std::getenv("Q4_ARENA_SCALE");
            return e ? static_cast<uint32_t>(std::atoi(e)) : 1u;
        }();
        if (scale > 1 && large && addr == 0 && allocSize >= 0xC000000 && allocSize <= 0xD000000)
            allocSize *= scale;
    }
    const GuestDomain domain = (type & mem::kPhysical) || (addr >= guest::kPhysicalBegin && addr < guest::kPhysicalEnd)
                                   ? GuestDomain::Physical
                                   : GuestDomain::User;

    if (type & mem::kReset)
    {
        // MEM_RESET only hints that the contents are no longer needed.
        ctx.r3.u64 = nt::kSuccess;
        return;
    }

    uint32_t result = 0;

    if (addr != 0)
    {
        GuestRegionInfo info;
        if (GuestQuery(addr, info) && info.state != kMemFree)
        {
            // Committing (part of) an existing reservation.
            if (type & mem::kCommit)
                result = GuestCommitRange(addr, allocSize) ? (addr & ~(pageSize - 1)) : 0;
            else
                result = addr & ~(pageSize - 1);
        }
        else
        {
            result = GuestAlloc(domain, addr, allocSize, pageSize, (type & mem::kCommit) != 0 || large);
        }
    }
    else
    {
        // A commit with no reservation behaves as reserve + commit. Large-page allocations are
        // always committed on the console, even when only MEM_RESERVE is requested.
        const bool commit = (type & mem::kCommit) != 0 || large;
        result = GuestAlloc(domain, 0, allocSize, pageSize, commit);
    }

    if (!result)
    {
        static int reported = 0;
        if (reported++ < 20)
        {
            std::printf("[alloc] NtAllocateVirtualMemory FAILED: base=0x%08X size=0x%X type=0x%X protect=0x%X domain=%d\n",
                        addr, size, type, ctx.r6.u32, static_cast<int>(domain));
            std::fflush(stdout);
        }
        ctx.r3.u64 = nt::kNoMemory;
        return;
    }

    if (result >= 0x38150000u && result < 0x44970000u && allocSize < 0x400000)
    {
        static int n = 0;
        if (n++ < 40)
        {
            std::printf("[alloc] arena3 commit: base=0x%08X size=0x%X type=0x%X\n", result, allocSize, type);
            std::fflush(stdout);
        }
    }

    if (allocSize >= 0x400000)
    {
        std::printf("[alloc] NtAllocateVirtualMemory ok: base=0x%08X size=0x%X type=0x%X domain=%d\n", result, allocSize,
                    type, static_cast<int>(domain));
        std::fflush(stdout);
    }

    GuestStore32(basePtr, result);
    GuestStore32(sizePtr, allocSize);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtFreeVirtualMemory(PVOID* base, PSIZE_T size, ULONG freeType, ULONG dbg)
PPC_FUNC(__imp__NtFreeVirtualMemory)
{
    const uint32_t basePtr = ctx.r3.u32;
    const uint32_t sizePtr = ctx.r4.u32;
    const uint32_t freeType = ctx.r5.u32;

    if (!basePtr)
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    const uint32_t addr = GuestLoad32(basePtr);
    const uint32_t size = sizePtr ? GuestLoad32(sizePtr) : 0;

    if (freeType & mem::kRelease)
    {
        uint32_t released = 0;
        if (!GuestRelease(addr, &released))
        {
            ctx.r3.u64 = nt::kInvalidParameter;
            return;
        }
        if (sizePtr)
            GuestStore32(sizePtr, released);
        ctx.r3.u64 = nt::kSuccess;
        return;
    }

    if (freeType & mem::kDecommit)
    {
        if (!GuestDecommitRange(addr, size))
        {
            ctx.r3.u64 = nt::kInvalidParameter;
            return;
        }
        ctx.r3.u64 = nt::kSuccess;
        return;
    }

    ctx.r3.u64 = nt::kInvalidParameter;
}

// NTSTATUS NtQueryVirtualMemory(PVOID base, MEMORY_BASIC_INFORMATION* info)
PPC_FUNC(__imp__NtQueryVirtualMemory)
{
    const uint32_t addr = ctx.r3.u32;
    const uint32_t out = ctx.r4.u32;

    GuestRegionInfo info;
    if (!out || !GuestQuery(addr, info))
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    const bool isFree = info.state == kMemFree;

    GuestStore32(out + 0x00, info.base);                                  // BaseAddress
    GuestStore32(out + 0x04, info.allocBase);                             // AllocationBase
    GuestStore32(out + 0x08, isFree ? 0 : kPageReadWrite);                // AllocationProtect
    GuestStore32(out + 0x0C, info.size);                                  // RegionSize
    GuestStore32(out + 0x10, info.state);                                 // State
    GuestStore32(out + 0x14, isFree ? kPageNoAccess : kPageReadWrite);    // Protect
    GuestStore32(out + 0x18, isFree ? 0 : kMemPrivate);                   // Type
    ctx.r3.u64 = nt::kSuccess;
}

// PVOID MmAllocatePhysicalMemoryEx(ULONG flags, SIZE_T size, ULONG protect, ULONG minAddr, ULONG maxAddr, ULONG align)
PPC_FUNC(__imp__MmAllocatePhysicalMemoryEx)
{
    const uint32_t size = ctx.r4.u32;
    uint32_t align = ctx.r8.u32;
    if (align < 0x1000)
        align = 0x1000;

    const uint32_t addr = GuestAlloc(GuestDomain::Physical, 0, AlignUp(size, 0x1000), align, true);
    if (size >= 0x400000 || !addr)
    {
        std::printf("[alloc] MmAllocatePhysicalMemoryEx size=0x%X align=0x%X -> 0x%08X\n", size, align, addr);
        std::fflush(stdout);
    }
    ctx.r3.u64 = addr;
}

// VOID MmFreePhysicalMemory(ULONG type, PVOID base)
PPC_FUNC(__imp__MmFreePhysicalMemory)
{
    GuestRelease(ctx.r4.u32, nullptr);
}

// ULONG MmGetPhysicalAddress(PVOID base)
PPC_FUNC(__imp__MmGetPhysicalAddress)
{
    const uint32_t addr = ctx.r3.u32;
    ctx.r3.u64 = addr >= guest::kPhysicalBegin ? (addr & 0x1FFFFFFF) : addr;
}

// ULONG MmQueryAddressProtect(PVOID base)
PPC_FUNC(__imp__MmQueryAddressProtect)
{
    GuestRegionInfo info;
    ctx.r3.u64 = (GuestQuery(ctx.r3.u32, info) && info.state != kMemFree) ? kPageReadWrite : kPageNoAccess;
}

// NTSTATUS MmQueryStatistics(MM_STATISTICS* out)
PPC_FUNC(__imp__MmQueryStatistics)
{
    const uint32_t out = ctx.r3.u32;
    if (!out)
    {
        ctx.r3.u64 = nt::kInvalidParameter;
        return;
    }

    constexpr uint32_t kTotalPages = 0x20000; // 512 MB of 4 KB pages
    GuestStore32(out + 0x00, 104);                  // structure size
    GuestStore32(out + 0x04, kTotalPages);          // total physical pages
    GuestStore32(out + 0x08, 0x300);                // kernel pages
    GuestStore32(out + 0x0C, kTotalPages - 0x600);  // title-available pages
    ctx.r3.u64 = nt::kSuccess;
}
