#include "watch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "backtrace.h"
#include "memory.h"
#include "thread.h"

namespace
{
    constexpr uint32_t kPageSize = 0x1000;
    constexpr uint32_t kTrapFlag = 0x100;
    constexpr uint32_t kSnapshotBytes = 64;

    struct Watch
    {
        uint32_t addr = 0;
        uint32_t size = 0;
        std::string label;
        unsigned reports = 0;
    };

    struct PageHook
    {
        uint32_t page;
        void (*callback)(uint32_t);
    };

    std::mutex g_mutex;
    std::vector<PageHook> g_pageHooks;
    std::vector<Watch> g_watches;
    std::unordered_map<uint32_t, bool> g_armedPages; // guest page index -> currently protected
    unsigned g_maxReports = 25;
    bool g_maxParsed = false;

    // State carried from the write fault to the single-step that follows it.
    struct Pending
    {
        bool valid = false;
        uint32_t page = 0;
        uintptr_t rip = 0;
        uint32_t faultAddr = 0;
        struct Snap
        {
            uint32_t watch;
            uint32_t len;
            uint8_t before[kSnapshotBytes];
        };
        std::vector<Snap> snaps;
    };
    thread_local Pending t_pending;

    uint8_t* GuestPtr(uint32_t addr) { return g_guestBase + addr; }

    bool PageCommitted(uint32_t page)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(GuestPtr(page * kPageSize), &mbi, sizeof(mbi)))
            return false;
        return mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) == 0;
    }

    // Must be called with g_mutex held.
    void ArmPage(uint32_t page)
    {
        auto& armed = g_armedPages[page];
        if (armed)
            return;
        if (!PageCommitted(page))
            return;

        DWORD old = 0;
        if (VirtualProtect(GuestPtr(page * kPageSize), kPageSize, PAGE_READONLY, &old))
        {
            armed = true;
            std::printf("[watch] page 0x%08X is now write-protected\n", page * kPageSize);
            std::fflush(stdout);
        }
    }

    // Must be called with g_mutex held.
    void ArmAll()
    {
        for (const auto& hk : g_pageHooks)
            ArmPage(hk.page);
        for (const auto& w : g_watches)
        {
            const uint32_t first = w.addr / kPageSize;
            const uint32_t last = (w.addr + w.size - 1) / kPageSize;
            for (uint32_t p = first; p <= last; p++)
                ArmPage(p);
        }
    }

    bool Overlaps(const Watch& w, uint32_t page)
    {
        const uint64_t pb = static_cast<uint64_t>(page) * kPageSize;
        return w.addr < pb + kPageSize && static_cast<uint64_t>(w.addr) + w.size > pb;
    }
}

void WatchInitFromEnv()
{
    if (const char* m = std::getenv("Q4_WATCH_MAX"))
    {
        g_maxReports = static_cast<unsigned>(std::atoi(m));
        g_maxParsed = true;
    }

    const char* spec = std::getenv("Q4_WATCH");
    if (!spec)
        return;

    std::string s(spec);
    size_t pos = 0;
    while (pos < s.size())
    {
        size_t end = s.find_first_of(",;", pos);
        if (end == std::string::npos)
            end = s.size();

        const std::string item = s.substr(pos, end - pos);
        pos = end + 1;
        if (item.empty())
            continue;

        const size_t colon = item.find(':');
        const uint32_t addr = static_cast<uint32_t>(std::strtoul(item.c_str(), nullptr, 0));
        const uint32_t size = colon == std::string::npos ? 4 : static_cast<uint32_t>(std::strtoul(item.c_str() + colon + 1, nullptr, 0));
        if (addr && size)
        {
            char label[48];
            std::snprintf(label, sizeof(label), "Q4_WATCH 0x%08X", addr);
            WatchAdd(addr, size, label);
        }
    }
}

uint32_t WatchAdd(uint32_t guestAddr, uint32_t size, const char* label)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    Watch w;
    w.addr = guestAddr;
    w.size = size ? size : 4;
    w.label = label ? label : "";
    g_watches.push_back(std::move(w));

    std::printf("[watch] #%zu armed on 0x%08X..0x%08X (%s)\n", g_watches.size() - 1, guestAddr, guestAddr + size, label ? label : "");
    std::fflush(stdout);

    ArmAll();
    return static_cast<uint32_t>(g_watches.size() - 1);
}

void WatchOnCommit(uint32_t guestAddr, uint32_t size)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_watches.empty() && g_pageHooks.empty())
        return;

    const uint32_t firstPage = guestAddr / kPageSize;
    const uint32_t lastPage = (guestAddr + (size ? size - 1 : 0)) / kPageSize;
    for (const auto& hk : g_pageHooks)
    {
        if (hk.page >= firstPage && hk.page <= lastPage)
        {
            g_armedPages[hk.page] = false;
            ArmPage(hk.page);
        }
    }

    for (const auto& w : g_watches)
    {
        for (uint32_t p = firstPage; p <= lastPage; p++)
        {
            if (!Overlaps(w, p))
                continue;

            // VirtualAlloc(MEM_COMMIT) over pages that are already committed resets their
            // protection to read/write, so forget the old state and protect them again.
            g_armedPages[p] = false;
            ArmPage(p);
        }
    }
}

bool WatchHandleException(EXCEPTION_POINTERS* info)
{
    const EXCEPTION_RECORD* rec = info->ExceptionRecord;

    // ---- the store that hit a protected page -----------------------------------------------
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && rec->ExceptionInformation[0] == 1)
    {
        const uintptr_t addr = static_cast<uintptr_t>(rec->ExceptionInformation[1]);
        const uintptr_t lo = reinterpret_cast<uintptr_t>(g_guestBase);
        if (!g_guestBase || addr < lo || addr >= lo + 0x100000000ull)
            return false;

        const uint32_t guestAddr = static_cast<uint32_t>(addr - lo);
        const uint32_t page = guestAddr / kPageSize;

        Pending pending;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            auto it = g_armedPages.find(page);
            if (it == g_armedPages.end() || !it->second)
                return false; // not one of ours

            pending.valid = true;
            pending.page = page;
            pending.rip = static_cast<uintptr_t>(info->ContextRecord->Rip);
            pending.faultAddr = guestAddr;

            for (uint32_t i = 0; i < g_watches.size(); i++)
            {
                const Watch& w = g_watches[i];
                if (!Overlaps(w, page))
                    continue;

                Pending::Snap s{};
                s.watch = i;
                s.len = w.size < kSnapshotBytes ? w.size : kSnapshotBytes;
                if (PageCommitted(w.addr / kPageSize))
                    std::memcpy(s.before, GuestPtr(w.addr), s.len);
                pending.snaps.push_back(s);
            }

            DWORD old = 0;
            VirtualProtect(GuestPtr(page * kPageSize), kPageSize, PAGE_READWRITE, &old);
            it->second = false;
        }

        static std::atomic<unsigned> trapCount{ 0 };
        const unsigned n = ++trapCount;
        if (n <= 12 || n % 5000 == 0)
        {
            std::printf("[watch-dbg] trap #%u: store to guest 0x%08X (page 0x%08X)\n", n, guestAddr, page * kPageSize);
            std::fflush(stdout);
        }

        t_pending = std::move(pending);
        info->ContextRecord->EFlags |= kTrapFlag;
        return true;
    }

    // ---- the single-step right after it ----------------------------------------------------
    if (rec->ExceptionCode == EXCEPTION_SINGLE_STEP && t_pending.valid)
    {
        info->ContextRecord->EFlags &= ~kTrapFlag;
        Pending pending = std::move(t_pending);
        t_pending = Pending();

        // Compare before re-arming (the page is still writable, which also keeps a rep-prefixed
        // store from faulting again on its next iteration).
        struct Report
        {
            uint32_t watch;
            uint32_t len;
            uint8_t before[kSnapshotBytes];
            uint8_t after[kSnapshotBytes];
        };
        std::vector<Report> reports;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto& s : pending.snaps)
            {
                Watch& w = g_watches[s.watch];
                if (!PageCommitted(w.addr / kPageSize))
                    continue;

                Report r{};
                r.watch = s.watch;
                r.len = s.len;
                std::memcpy(r.before, s.before, s.len);
                std::memcpy(r.after, GuestPtr(w.addr), s.len);
                if (std::memcmp(r.before, r.after, s.len) != 0 && w.reports < g_maxReports)
                {
                    w.reports++;
                    reports.push_back(r);
                }
            }

            // Re-arm.
            DWORD old = 0;
            if (VirtualProtect(GuestPtr(pending.page * kPageSize), kPageSize, PAGE_READONLY, &old))
                g_armedPages[pending.page] = true;
        }

        {
            void (*cb)(uint32_t) = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                for (const auto& hk : g_pageHooks)
                    if (hk.page == pending.page)
                        cb = hk.callback;
            }
            if (cb)
                cb(pending.faultAddr);
        }

        for (const auto& r : reports)
        {
            std::string label;
            uint32_t addr = 0;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                label = g_watches[r.watch].label;
                addr = g_watches[r.watch].addr;
            }

            std::printf("[watch] #%u write to 0x%08X (%s), writer: %s\n", r.watch, addr, label.c_str(),
                        ResolveHostRip(pending.rip).c_str());

            // Show the bytes that changed as big-endian dwords where possible.
            for (uint32_t off = 0; off + 4 <= r.len; off += 4)
            {
                uint32_t b, a;
                std::memcpy(&b, r.before + off, 4);
                std::memcpy(&a, r.after + off, 4);
                if (b != a)
                    std::printf("        +0x%02X: 0x%08X -> 0x%08X\n", off, _byteswap_ulong(b), _byteswap_ulong(a));
            }
            BacktraceDumpCurrentThread("write watchpoint");
        }
        return true;
    }

    return false;
}

void WatchAddPageHook(uint32_t guestAddr, void (*callback)(uint32_t writtenAddr))
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_pageHooks.push_back({ guestAddr / kPageSize, callback });
    ArmPage(guestAddr / kPageSize);
}
