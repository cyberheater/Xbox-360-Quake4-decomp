#include "memory.h"
#include <cstdio>
#include <windows.h>

#include "ppc_config.h"
#include "ppc_context.h"

uint8_t* g_guestBase = nullptr;
bool g_guestPhysicalAliased = false;

namespace
{
    // The console exposes its 512 MB of physical RAM through three aliases:
    //   0xA0000000  cached            0xC0000000  uncached / write-combined
    //   0xE0000000  cached, 4 KB pages
    // They are views of the same bytes, so they must be views of one section on the host.
    constexpr uint64_t kGuestSpace = 0x100000000ull;
    constexpr uint64_t kPhysicalSize = 0x20000000ull;
    constexpr uint64_t kAliasBases[3] = { 0xA0000000ull, 0xC0000000ull, 0xE0000000ull };

#ifndef MEM_PRESERVE_PLACEHOLDER
    constexpr ULONG MEM_PRESERVE_PLACEHOLDER_ = 0x00000002;
    constexpr ULONG MEM_REPLACE_PLACEHOLDER_ = 0x00004000;
    constexpr ULONG MEM_RESERVE_PLACEHOLDER_ = 0x00040000;
#else
    constexpr ULONG MEM_PRESERVE_PLACEHOLDER_ = MEM_PRESERVE_PLACEHOLDER;
    constexpr ULONG MEM_REPLACE_PLACEHOLDER_ = MEM_REPLACE_PLACEHOLDER;
    constexpr ULONG MEM_RESERVE_PLACEHOLDER_ = MEM_RESERVE_PLACEHOLDER;
#endif

    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, PVOID, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG, PVOID, ULONG);

    // Builds the guest space with the physical aliases mapped. Returns null if the placeholder
    // APIs are unavailable or any step fails (the caller then falls back to a plain reservation).
    uint8_t* ReserveWithPhysicalAliases()
    {
        HMODULE kernelBase = GetModuleHandleA("kernelbase.dll");
        if (!kernelBase)
            return nullptr;

        auto virtualAlloc2 = reinterpret_cast<VirtualAlloc2Fn>(GetProcAddress(kernelBase, "VirtualAlloc2"));
        auto mapViewOfFile3 = reinterpret_cast<MapViewOfFile3Fn>(GetProcAddress(kernelBase, "MapViewOfFile3"));
        if (!virtualAlloc2 || !mapViewOfFile3)
        {
            std::fprintf(stderr, "[mem] VirtualAlloc2/MapViewOfFile3 not exported by kernelbase.dll\n");
            return nullptr;
        }

        HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            static_cast<DWORD>(kPhysicalSize), nullptr);
        if (!section)
        {
            std::fprintf(stderr, "[mem] CreateFileMapping(512 MB) failed, error %lu\n", GetLastError());
            return nullptr;
        }

        HANDLE self = GetCurrentProcess();
        auto* base = static_cast<uint8_t*>(
            virtualAlloc2(self, nullptr, kGuestSpace, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER_, PAGE_NOACCESS, nullptr, 0));
        if (!base)
        {
            std::fprintf(stderr, "[mem] 4 GB placeholder reservation failed, error %lu\n", GetLastError());
            CloseHandle(section);
            return nullptr;
        }

        auto fail = [&]() -> uint8_t*
        {
            VirtualFree(base, 0, MEM_RELEASE);
            CloseHandle(section);
            return nullptr;
        };

        // Carve the three alias windows out of the placeholder, then map the section into each.
        // The last window ends where the space ends, so after the first two splits it already is
        // a placeholder of exactly its own size and must not be split again.
        for (uint64_t alias : kAliasBases)
        {
            if (alias + kPhysicalSize >= kGuestSpace)
                continue;

            if (!VirtualFree(base + alias, static_cast<SIZE_T>(kPhysicalSize), MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER_))
            {
                std::fprintf(stderr, "[mem] splitting placeholder at 0x%llX failed, error %lu\n",
                             static_cast<unsigned long long>(alias), GetLastError());
                return fail();
            }
        }

        for (uint64_t alias : kAliasBases)
        {
            if (!mapViewOfFile3(section, self, base + alias, 0, static_cast<SIZE_T>(kPhysicalSize),
                                MEM_REPLACE_PLACEHOLDER_, PAGE_READWRITE, nullptr, 0))
            {
                std::fprintf(stderr, "[mem] mapping physical view at 0x%llX failed, error %lu\n",
                             static_cast<unsigned long long>(alias), GetLastError());
                return fail();
            }
        }

        // What remains is one placeholder covering [0, 0xA0000000); turn it into an ordinary
        // reservation so the guest allocator can commit pages in it with VirtualAlloc.
        if (!virtualAlloc2(self, base, static_cast<SIZE_T>(kAliasBases[0]), MEM_RESERVE | MEM_REPLACE_PLACEHOLDER_,
                           PAGE_READWRITE, nullptr, 0))
        {
            std::fprintf(stderr, "[mem] converting low placeholder to a reservation failed, error %lu\n", GetLastError());
            return fail();
        }

        // 0xC0000000 - 0xE0000000 was mapped above; the gap between 0xA0000000+512M and 0xC0000000
        // (and after 0xE0000000+512M) is covered by the alias windows themselves, so nothing else
        // is left over.
        return base;
    }
}

uint8_t* GuestMemoryInit()
{
    uint8_t* base = ReserveWithPhysicalAliases();
    if (base)
    {
        g_guestPhysicalAliased = true;
        std::printf("[mem] guest space reserved with physical aliases at A0000000/C0000000/E0000000\n");
    }
    else
    {
        std::fprintf(stderr, "[mem] placeholder aliasing unavailable; falling back to a plain reservation\n");
        base = static_cast<uint8_t*>(VirtualAlloc(nullptr, PPC_MEMORY_SIZE, MEM_RESERVE, PAGE_READWRITE));
    }

    if (!base)
        std::fprintf(stderr, "[mem] failed to reserve %llu bytes of guest address space (err %lu)\n",
                     static_cast<unsigned long long>(PPC_MEMORY_SIZE), GetLastError());
    g_guestBase = base;
    return base;
}

bool GuestCommit(uint8_t* base, uint32_t guestAddr, size_t size)
{
    void* p = VirtualAlloc(base + guestAddr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p)
        std::fprintf(stderr, "[mem] commit failed at 0x%08X size 0x%zX (err %lu)\n", guestAddr, size, GetLastError());
    return p != nullptr;
}
