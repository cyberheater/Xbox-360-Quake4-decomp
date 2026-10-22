// Function-entry probe: records the first call of every recompiled function (order, time,
// thread) so the sequence of what the title executes can be read back.
#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>
#include <windows.h>

#include "thread.h"

extern "C" unsigned char g_q4Seen[0x1B70000 / 4 + 16] = {};
extern "C" unsigned int g_q4Count[0x1B70000 / 4 + 16] = {};

namespace
{
    struct Entry
    {
        uint32_t addr;
        uint32_t thread;
        uint32_t ms;
    };

    std::mutex g_mutex;
    std::vector<Entry> g_entries;
}

extern "C" void Q4ProbeSlow(unsigned int addr)
{
    GuestThread* t = CurrentGuestThread();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_q4Seen[(addr - 0x82000000u) >> 2])
        return;
    g_q4Seen[(addr - 0x82000000u) >> 2] = 1;
    g_entries.push_back({ addr, t ? t->id : 0, static_cast<uint32_t>(GetTickCount64()) });
}

// Writes every first-called function in order. Returns the count.
size_t ProbeDump(const char* path)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    FILE* f = std::fopen(path, "w");
    if (!f)
        return 0;
    const uint32_t t0 = g_entries.empty() ? 0 : g_entries.front().ms;
    for (size_t i = 0; i < g_entries.size(); i++)
        std::fprintf(f, "%zu %08X t%u +%ums count=%u\n", i, g_entries[i].addr, g_entries[i].thread, g_entries[i].ms - t0, g_q4Count[(g_entries[i].addr - 0x82000000u) >> 2]);
    std::fclose(f);
    return g_entries.size();
}
