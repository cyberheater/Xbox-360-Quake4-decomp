#include "imports.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <windows.h>

#include "memory.h"
#include "xex.h"

void StartTimeStampUpdater(uint32_t guestAddr);

namespace
{
    std::mutex g_kernelMutex;
    uint32_t g_kernelNext = guest::kKernelBase;

    uint32_t AllocKernelData(uint32_t size) { return KernelAlloc(size); }

    void StoreBE32(uint8_t* base, uint32_t guestAddr, uint32_t value)
    {
        *reinterpret_cast<uint32_t*>(base + guestAddr) = _byteswap_ulong(value);
    }

    // Size of the storage backing each kernel *variable* export. Zero-filled by default.
    uint32_t VariableSize(const std::string& name)
    {
        if (name == "__imp__KeTimeStampBundle")
            return 0x20;
        if (name == "__imp__XexExecutableModuleHandle")
            return 0x4; // holds a pointer to the module's LDR entry (see below)
        return 0x40;
    }
}

uint32_t g_xexModuleHandle = 0;

// KeTimeStampBundle is a block of kernel memory the title reads directly (no import call), so
// something has to keep it current. Layout (big endian):
//   +0x00 u64 InterruptTime  100 ns units since boot
//   +0x08 u64 SystemTime     FILETIME (100 ns units since 1601)
//   +0x10 u32 TickCount      milliseconds since boot
// Engines use the tick count for fades, timeouts and frame timing, so a frozen value stalls them.
void StartTimeStampUpdater(uint32_t guestAddr)
{
    std::thread([guestAddr]()
    {
        LARGE_INTEGER freq, start;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&start);

        for (;;)
        {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);

            const uint64_t elapsed100ns =
                static_cast<uint64_t>((now.QuadPart - start.QuadPart) * 10000000ll / freq.QuadPart);

            FILETIME ft;
            GetSystemTimeAsFileTime(&ft);
            const uint64_t systemTime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;

            GuestStore64(guestAddr + 0x00, elapsed100ns);
            GuestStore64(guestAddr + 0x08, systemTime);
            GuestStore32(guestAddr + 0x10, static_cast<uint32_t>(elapsed100ns / 10000));

            Sleep(1);
        }
    }).detach();
}

uint32_t KernelAlloc(uint32_t size)
{
    std::lock_guard<std::mutex> lock(g_kernelMutex);

    const uint32_t addr = g_kernelNext;
    g_kernelNext += (size + 0x3F) & ~0x3Fu;
    if (g_kernelNext > guest::kKernelBase + guest::kKernelSize)
    {
        std::fprintf(stderr, "[imports] kernel data region exhausted\n");
        std::exit(1);
    }
    return addr;
}

ImportFixupStats FixupImports(uint8_t* base)
{
    ImportFixupStats stats;

    // (library, ordinal) -> thunk guest address, for function imports.
    std::map<std::pair<std::string, uint32_t>, uint32_t> thunks;
    for (const auto& r : g_Xex2ImportRecords)
        if (r.type == 1)
            thunks[{ r.library, r.ordinal }] = r.slot;

    for (const auto& r : g_Xex2ImportRecords)
    {
        if (r.type != 0)
            continue;

        auto thunk = thunks.find({ r.library, r.ordinal });
        if (thunk != thunks.end())
        {
            StoreBE32(base, r.slot, thunk->second);
            stats.functions++;
            continue;
        }

        // Variable import: give it guest storage and point the IAT slot at it.
        const std::string name = r.name.empty() ? "<unnamed>" : r.name;
        const uint32_t storage = AllocKernelData(VariableSize(name));
        StoreBE32(base, r.slot, storage);
        stats.variables++;

        if (name == "__imp__KeTimeStampBundle")
            StartTimeStampUpdater(storage);

        if (name == "__imp__XexExecutableModuleHandle")
        {
            // The guest loads the handle through the variable, then reads the XEX header base
            // from +0x58 of the LDR entry. We keep the entry zeroed: header lookups go through
            // RtlImageXexHeaderField, which is implemented natively.
            g_xexModuleHandle = AllocKernelData(0x100);
            StoreBE32(base, storage, g_xexModuleHandle);
        }

        std::printf("[imports] variable %-34s slot=0x%08X -> storage 0x%08X\n", name.c_str(), r.slot, storage);
    }

    return stats;
}
