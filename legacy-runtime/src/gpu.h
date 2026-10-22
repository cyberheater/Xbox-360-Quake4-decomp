#pragma once
#include <cstddef>
#include <cstdint>

// Xenos command processor (PM4 interpreter). It consumes the ring buffer the title publishes
// through VdInitializeRingBuffer + the CP_RB_WPTR register, executes the packets that matter to
// the CPU side (register writes, fences, memory writes, interrupts) and reports progress through
// the read-pointer write-back address. Drawing is not implemented yet.

// Ring buffer location: `physBase` is a physical address, the size is 8 << sizeLog2 bytes.
void GpuCpSetRing(uint32_t physBase, uint32_t sizeLog2);

// Where the processor publishes its read pointer (physical address).
void GpuCpSetWriteback(uint32_t physAddr);

// Starts the processor thread (idempotent). Needs the ring and write-back to be configured.
void GpuCpStart();

// Statistics for diagnostics.
struct GpuCpStats
{
    uint32_t packets = 0;
    uint32_t type0 = 0;
    uint32_t type3 = 0;
    uint32_t indirectBuffers = 0;
    uint32_t memoryWrites = 0;
    uint32_t interrupts = 0;
    uint32_t swaps = 0;
    uint32_t unknownOpcodes = 0;

    // Live position, for diagnosing a stalled processor.
    uint32_t rptr = 0;          // ring read pointer (dwords) the processor has completed
    uint32_t curHeader = 0;     // header of the packet being executed
    uint32_t curDepth = 0;      // indirect-buffer nesting of that packet
    uint32_t curOpcode = 0;     // type-3 opcode (0xFF for other packet types)
    bool busy = false;          // inside ExecuteBuffer
};
GpuCpStats GpuCpGetStats();

// True when the processor has executed everything the title has submitted: it is not inside a
// packet and its read pointer equals the write pointer register.
bool GpuCpIsIdle();

// Describes the packet the calling (processor) thread is executing: buffer, position, header.
void GpuCpDescribeCurrent(char* out, size_t outSize);

// Prints `before` dwords preceding and `after` dwords following the current packet.
void GpuCpDumpAroundCurrent(int before, int after);

// Decodes the packet headers from `back` dwords before the current packet up to it (the start must
// be a packet boundary).
void GpuCpDecodeBefore(int back);

// Describes the scratch-register mirroring state (UMSK, ADDR, last mirrored values).
void GpuCpDescribeScratch(char* out, size_t outSize);

// Marks a presented frame for the draw tracker.
void GpuCpFrameEnd();

// Provided by the video module: delivers a graphics interrupt to the guest callback.
struct PPCContext;
void VideoDispatchInterrupt(PPCContext& ctx, uint32_t source);

// Called by the processor when the title issues a swap.
void VideoNotifySwap();

// Waits (bounded) until the command processor has consumed everything submitted so far.
void GpuCpWaitDrained(uint32_t timeoutMs);
