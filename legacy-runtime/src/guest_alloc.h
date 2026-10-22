#pragma once
#include <cstdint>

// Address-range allocator for the guest's 4 GB space. Ranges are reserved virtually (the whole
// space is already reserved on the host); pages are committed on demand.
enum class GuestDomain
{
    User,      // [kUserBegin, kUserEnd)       general virtual memory
    Stack,     // [kStackBegin, kStackEnd)     thread stacks
    Physical,  // [kPhysicalBegin, kPhysicalEnd) physically-addressed (GPU) memory
};

struct GuestRegionInfo
{
    uint32_t base = 0;        // start of the queried page range
    uint32_t allocBase = 0;   // base of the allocation that contains it
    uint32_t size = 0;        // size of the run of pages sharing the same state
    uint32_t state = 0;       // MEM_COMMIT / MEM_RESERVE / MEM_FREE
};

constexpr uint32_t kGuestPageSize = 0x1000;

// Reserves (and optionally commits) `size` bytes. `desired` == 0 lets the allocator choose.
// Returns the guest base address, or 0 on failure. Memory is zero-filled when committed.
uint32_t GuestAlloc(GuestDomain domain, uint32_t desired, uint32_t size, uint32_t alignment, bool commit);

// Commits pages inside an existing reservation (or reserves+commits a fixed free range).
bool GuestCommitRange(uint32_t addr, uint32_t size);

// Returns the pages to the reserved-but-uncommitted state.
bool GuestDecommitRange(uint32_t addr, uint32_t size);

// Releases a whole allocation. `addr` must be the allocation base (or 0 size + base).
bool GuestRelease(uint32_t addr, uint32_t* releasedSize);

// Describes the page at `addr`.
bool GuestQuery(uint32_t addr, GuestRegionInfo& out);

// Size in bytes of the allocation that starts at `addr` (0 if none).
uint32_t GuestAllocationSize(uint32_t addr);
