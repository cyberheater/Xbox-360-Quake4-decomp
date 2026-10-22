#pragma once
#include <cstdint>

// NTSTATUS values used by the kernel layer.
namespace nt
{
    constexpr uint32_t kSuccess              = 0x00000000;
    constexpr uint32_t kUserApc              = 0x000000C0;
    constexpr uint32_t kTimeout              = 0x00000102;
    constexpr uint32_t kPending              = 0x00000103;
    constexpr uint32_t kUnsuccessful         = 0xC0000001;
    constexpr uint32_t kNotImplemented       = 0xC0000002;
    constexpr uint32_t kInvalidHandle        = 0xC0000008;
    constexpr uint32_t kInvalidParameter     = 0xC000000D;
    constexpr uint32_t kNoSuchFile           = 0xC000000F;
    constexpr uint32_t kNoMemory             = 0xC0000017;
    constexpr uint32_t kAccessViolation      = 0xC0000005;
    constexpr uint32_t kConflictingAddresses = 0xC0000018;
    constexpr uint32_t kNotFound             = 0xC0000225;
    constexpr uint32_t kObjectNameNotFound   = 0xC0000034;
    constexpr uint32_t kEndOfFile            = 0xC0000011;
    constexpr uint32_t kBufferOverflow       = 0x80000005;
    constexpr uint32_t kNoMoreFiles          = 0x80000006;
}

// Win32-style allocation / protection flags the guest passes to NtAllocateVirtualMemory.
namespace mem
{
    constexpr uint32_t kCommit      = 0x1000;
    constexpr uint32_t kReserve     = 0x2000;
    constexpr uint32_t kDecommit    = 0x4000;
    constexpr uint32_t kRelease     = 0x8000;
    constexpr uint32_t kReset       = 0x80000;
    constexpr uint32_t kTopDown     = 0x100000;
    constexpr uint32_t kPhysical    = 0x400000;
    constexpr uint32_t kLargePages  = 0x20000000;
    constexpr uint32_t k16MbPages   = 0x40000000;
    constexpr uint32_t k4MbPages    = 0x80000000;
}
