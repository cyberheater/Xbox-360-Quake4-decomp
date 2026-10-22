#pragma once
#include <cstdint>
#include <cstddef>
#include <intrin.h>

// Guest address space layout (all values are guest virtual addresses).
namespace guest
{
    // Regions managed by the guest allocator (see guest_alloc.h).
    constexpr uint32_t kUserBegin     = 0x20000000;  // NtAllocateVirtualMemory
    constexpr uint32_t kUserEnd       = 0x70000000;
    constexpr uint32_t kStackBegin    = 0x70000000;  // thread stacks
    constexpr uint32_t kStackEnd      = 0x7FC00000;
    constexpr uint32_t kPhysicalBegin = 0xA0000000;  // MmAllocatePhysicalMemoryEx (GPU-visible)
    constexpr uint32_t kPhysicalEnd   = 0xC0000000;

    // Fixed regions.
    constexpr uint32_t kGpuRegBase = 0x7FC80000;     // Xenos MMIO registers (plain memory for now)
    constexpr uint32_t kGpuRegSize = 0x00080000;
    constexpr uint32_t kKernelBase = 0x7FE00000;     // kernel variable exports, PCRs, fake KTHREADs, ...
    constexpr uint32_t kKernelSize = 0x00100000;
}

// Host pointer to guest address 0. Set once by GuestMemoryInit().
extern uint8_t* g_guestBase;

// True when 0xA0000000 / 0xC0000000 / 0xE0000000 are views of one backing section (real physical
// aliasing). When false the physical windows are ordinary, independent memory.
extern bool g_guestPhysicalAliased;

// Reserves the full 4 GB guest address space and returns the host base pointer.
uint8_t* GuestMemoryInit();

// Commits [guestAddr, guestAddr + size) read/write. Returns false on failure.
bool GuestCommit(uint8_t* base, uint32_t guestAddr, size_t size);

// Big-endian accessors for guest memory.
inline uint16_t GuestLoad16(uint32_t addr) { return _byteswap_ushort(*reinterpret_cast<uint16_t*>(g_guestBase + addr)); }
inline uint32_t GuestLoad32(uint32_t addr) { return _byteswap_ulong(*reinterpret_cast<uint32_t*>(g_guestBase + addr)); }
inline uint64_t GuestLoad64(uint32_t addr) { return _byteswap_uint64(*reinterpret_cast<uint64_t*>(g_guestBase + addr)); }
inline void GuestStore16(uint32_t addr, uint16_t v) { *reinterpret_cast<uint16_t*>(g_guestBase + addr) = _byteswap_ushort(v); }
inline void GuestStore32(uint32_t addr, uint32_t v) { *reinterpret_cast<uint32_t*>(g_guestBase + addr) = _byteswap_ulong(v); }
inline void GuestStore64(uint32_t addr, uint64_t v) { *reinterpret_cast<uint64_t*>(g_guestBase + addr) = _byteswap_uint64(v); }
