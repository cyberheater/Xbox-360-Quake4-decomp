#include "guest_alloc.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <windows.h>

#include "memory.h"
#include "watch.h"

namespace
{
    struct Block
    {
        uint32_t size;
    };

    std::mutex g_mutex;
    std::map<uint32_t, Block> g_blocks; // allocation base -> block

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    void DomainRange(GuestDomain d, uint32_t& begin, uint32_t& end)
    {
        switch (d)
        {
        case GuestDomain::Stack:    begin = guest::kStackBegin;    end = guest::kStackEnd;    break;
        case GuestDomain::Physical: begin = guest::kPhysicalBegin; end = guest::kPhysicalEnd; break;
        default:                    begin = guest::kUserBegin;     end = guest::kUserEnd;     break;
        }
    }

    // Returns the block containing `addr`, or end().
    std::map<uint32_t, Block>::iterator FindContaining(uint32_t addr)
    {
        auto it = g_blocks.upper_bound(addr);
        if (it == g_blocks.begin())
            return g_blocks.end();
        --it;
        if (addr < it->first + it->second.size)
            return it;
        return g_blocks.end();
    }

    bool Overlaps(uint32_t begin, uint32_t size)
    {
        auto it = g_blocks.upper_bound(begin);
        if (it != g_blocks.begin())
        {
            auto prev = std::prev(it);
            if (prev->first + prev->second.size > begin)
                return true;
        }
        return it != g_blocks.end() && it->first < begin + size;
    }

    // The physical-memory windows (0xA0000000 and up) are views of an already committed section;
    // "committing" them just hands back zeroed bytes.
    bool IsPhysicalWindow(uint32_t addr) { return g_guestPhysicalAliased && addr >= guest::kPhysicalBegin; }

    bool HostCommit(uint32_t addr, uint32_t size)
    {
        if (IsPhysicalWindow(addr))
        {
            std::memset(g_guestBase + addr, 0, size);
            WatchOnCommit(addr, size);
            return true;
        }

        if (VirtualAlloc(g_guestBase + addr, size, MEM_COMMIT, PAGE_READWRITE) == nullptr)
            return false;

        // Memory that just became real may carry a pending write watchpoint.
        WatchOnCommit(addr, size);
        return true;
    }
}

uint32_t GuestAlloc(GuestDomain domain, uint32_t desired, uint32_t size, uint32_t alignment, bool commit)
{
    if (size == 0)
        return 0;

    if (alignment < kGuestPageSize)
        alignment = kGuestPageSize;
    size = AlignUp(size, kGuestPageSize);

    std::lock_guard<std::mutex> lock(g_mutex);

    uint32_t rangeBegin, rangeEnd;
    DomainRange(domain, rangeBegin, rangeEnd);

    uint32_t base = 0;

    if (desired != 0)
    {
        desired &= ~(kGuestPageSize - 1);
        if (Overlaps(desired, size))
            return 0;
        base = desired;
    }
    else
    {
        // First fit between existing blocks.
        uint32_t cursor = AlignUp(rangeBegin, alignment);
        for (auto it = g_blocks.lower_bound(rangeBegin); it != g_blocks.end() && it->first < rangeEnd; ++it)
        {
            if (it->first >= cursor && it->first - cursor >= size)
                break;
            cursor = AlignUp(it->first + it->second.size, alignment);
        }

        if (cursor < rangeBegin || static_cast<uint64_t>(cursor) + size > rangeEnd)
        {
            static int reported = 0;
            if (reported++ < 20)
            {
                uint64_t used = 0;
                for (auto it = g_blocks.lower_bound(rangeBegin); it != g_blocks.end() && it->first < rangeEnd; ++it)
                    used += it->second.size;
                std::printf("[alloc] FAILED: domain %d size 0x%X align 0x%X (range 0x%08X-0x%08X, %u blocks, 0x%llX bytes used)\n", static_cast<int>(domain), size, alignment, rangeBegin, rangeEnd,
                            static_cast<unsigned>(g_blocks.size()), static_cast<unsigned long long>(used));
                std::fflush(stdout);
            }
            return 0;
        }
        base = cursor;
    }

    if (commit && !HostCommit(base, size))
        return 0;

    g_blocks[base] = Block{ size };
    return base;
}

bool GuestCommitRange(uint32_t addr, uint32_t size)
{
    if (size == 0)
        return false;

    const uint32_t begin = addr & ~(kGuestPageSize - 1);
    const uint32_t end = AlignUp(addr + size, kGuestPageSize);

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = FindContaining(begin);
    if (it == g_blocks.end())
        return false;
    if (end > it->first + it->second.size)
        return false;

    return HostCommit(begin, end - begin);
}

bool GuestDecommitRange(uint32_t addr, uint32_t size)
{
    const uint32_t begin = addr & ~(kGuestPageSize - 1);
    const uint32_t end = AlignUp(addr + size, kGuestPageSize);

    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = FindContaining(begin);
    if (it == g_blocks.end() || end > it->first + it->second.size)
        return false;

    if (IsPhysicalWindow(begin))
        return true; // section views stay backed; the next allocation re-zeroes them

    return VirtualFree(g_guestBase + begin, end - begin, MEM_DECOMMIT) != 0;
}

bool GuestRelease(uint32_t addr, uint32_t* releasedSize)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = g_blocks.find(addr);
    if (it == g_blocks.end())
        return false;

    if (releasedSize)
        *releasedSize = it->second.size;

    // Decommit rather than VirtualFree(MEM_RELEASE): the whole 4 GB is one host reservation.
    if (!IsPhysicalWindow(it->first))
        VirtualFree(g_guestBase + it->first, it->second.size, MEM_DECOMMIT);
    g_blocks.erase(it);
    return true;
}

bool GuestQuery(uint32_t addr, GuestRegionInfo& out)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto it = FindContaining(addr);
    if (it == g_blocks.end())
    {
        out.base = addr & ~(kGuestPageSize - 1);
        out.allocBase = 0;
        out.size = kGuestPageSize;
        out.state = 0x10000; // MEM_FREE
        return true;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(g_guestBase + addr, &mbi, sizeof(mbi)))
        return false;

    const uint32_t blockEnd = it->first + it->second.size;
    uint32_t runEnd = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize - g_guestBase);
    if (runEnd > blockEnd)
        runEnd = blockEnd;

    out.base = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(mbi.BaseAddress) - g_guestBase);
    out.allocBase = it->first;
    out.size = runEnd - out.base;
    out.state = mbi.State == MEM_COMMIT ? 0x1000 : 0x2000;
    return true;
}

uint32_t GuestAllocationSize(uint32_t addr)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_blocks.find(addr);
    return it == g_blocks.end() ? 0 : it->second.size;
}
