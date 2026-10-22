// xboxkrnl Vd* (video driver) exports and XGetVideoMode.
//
// There is no GPU behind these yet: they report a fixed 1280x720 / 60 Hz widescreen HD display,
// remember the ring-buffer and callback addresses the guest hands over, and emulate the vblank
// interrupt so the title's graphics-callback driven logic keeps running.
#include "import_stubs.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "backtrace.h"
#include "gpu.h"
#include "renderer.h"
#include "guest_alloc.h"
#include "kstatus.h"
#include "memory.h"
#include "thread.h"

static void CallInterruptHandler(PPCContext& ctx, uint32_t source);

namespace
{
    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;

    // State handed over by the guest; consumed by the (future) command processor.
    struct VideoState
    {
        std::atomic<uint32_t> interruptCallback{ 0 };
        std::atomic<uint32_t> interruptContext{ 0 };
        std::atomic<bool> vblankRunning{ false };

        uint32_t ringBase = 0;
        uint32_t ringSizeLog2 = 0;
        uint32_t rptrWritebackAddr = 0;
        uint32_t rptrWritebackBlock = 0;
        uint32_t gpuIdentifierAddr = 0;
        uint32_t systemCommandBuffer = 0;

        std::atomic<uint32_t> swapCount{ 0 };
    } g_video;

    uint32_t FloatBits(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, sizeof(u));
        return u;
    }

    // X_VIDEO_MODE (0x30 bytes)
    void FillVideoMode(uint32_t ptr)
    {
        if (!ptr)
            return;

        std::memset(g_guestBase + ptr, 0, 0x30);
        GuestStore32(ptr + 0x00, kWidth);             // display width
        GuestStore32(ptr + 0x04, kHeight);            // display height
        GuestStore32(ptr + 0x08, 0);                  // is interlaced
        GuestStore32(ptr + 0x0C, 1);                  // is widescreen
        GuestStore32(ptr + 0x10, 1);                  // is HD
        GuestStore32(ptr + 0x14, FloatBits(60.0f));   // refresh rate
        GuestStore32(ptr + 0x18, 1);                  // video standard (NTSC-M)
        GuestStore32(ptr + 0x1C, 0x4A);               // unknown, value used by retail kernels
        GuestStore32(ptr + 0x20, 0x01);               // unknown
    }

    void StartVblankThread()
    {
        bool expected = false;
        if (!g_video.vblankRunning.compare_exchange_strong(expected, true))
            return;

        if (std::getenv("Q4_NO_VBLANK"))
            return;

        static const int vblankUs = std::getenv("Q4_VBLANK_US") ? std::atoi(std::getenv("Q4_VBLANK_US")) : 16667;
        CreateGuestWorker([](PPCContext& ctx)
        {
            using clock = std::chrono::steady_clock;
            auto next = clock::now();

            for (;;)
            {
                next += std::chrono::microseconds(vblankUs);
                std::this_thread::sleep_until(next);

                // The display only starts raising vblank once the title has presented a frame;
                // before that the callback would run against half-initialised device state.
                if (!g_video.swapCount.load())
                    continue;

                // While the command stream has a CPU-interrupt callback armed in scratch register 4
                // (between its set and clear), a vblank must not run: the handler would consume
                // the callback and poison the slot before the stream's own INTERRUPT arrives.
                const uint32_t armed = GuestLoad32(guest::kGpuRegBase + 0x057C * 4);
                if (armed != 0x0BADF00D && armed != 0)
                    continue;

                CallInterruptHandler(ctx, 0); // source 0 = vblank
            }
        });
    }
}

// VOID XGetVideoMode(XVIDEO_MODE* out)
PPC_FUNC(__imp__XGetVideoMode)
{
    TraceImport("XGetVideoMode", ctx);
    FillVideoMode(ctx.r3.u32);
}

// VOID VdQueryVideoMode(XVIDEO_MODE* out)
PPC_FUNC(__imp__VdQueryVideoMode)
{
    TraceImport("VdQueryVideoMode", ctx);
    FillVideoMode(ctx.r3.u32);
}

// ULONG VdQueryVideoFlags()
PPC_FUNC(__imp__VdQueryVideoFlags)
{
    ctx.r3.u64 = 0x00000006; // HD + widescreen
}

// VOID VdGetCurrentDisplayInformation(X_DISPLAY_INFO* out)
PPC_FUNC(__imp__VdGetCurrentDisplayInformation)
{
    TraceImport("VdGetCurrentDisplayInformation", ctx);

    const uint32_t ptr = ctx.r3.u32;
    if (!ptr)
        return;

    std::memset(g_guestBase + ptr, 0, 0x58);
    GuestStore16(ptr + 0x00, kWidth);   // front buffer width
    GuestStore16(ptr + 0x02, kHeight);  // front buffer height
    GuestStore32(ptr + 0x0C, 0x1E);     // display format (k_8_8_8_8)
    GuestStore32(ptr + 0x10, kWidth);   // display width
    GuestStore32(ptr + 0x14, kHeight);  // display height
    GuestStore32(ptr + 0x18, FloatBits(60.0f));
}

// VOID VdGetCurrentDisplayGamma(ULONG* type, FLOAT* gamma)
PPC_FUNC(__imp__VdGetCurrentDisplayGamma)
{
    if (ctx.r3.u32)
        GuestStore32(ctx.r3.u32, 2);                       // gamma type
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, FloatBits(2.22222233f));  // gamma value
}

// ULONG VdGetGraphicsAsicID()
PPC_FUNC(__imp__VdGetGraphicsAsicID)
{
    ctx.r3.u64 = 0x00000002;
}

// BOOLEAN VdIsHSIOTrainingSucceeded()
PPC_FUNC(__imp__VdIsHSIOTrainingSucceeded)
{
    ctx.r3.u64 = 1;
}

PPC_FUNC(__imp__VdSetDisplayMode)
{
    TraceImport("VdSetDisplayMode", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdEnableDisableClockGating)
{
}

PPC_FUNC(__imp__VdInitializeEDRAM)
{
    TraceImport("VdInitializeEDRAM", ctx);
}

PPC_FUNC(__imp__VdRetrainEDRAM)
{
    TraceImport("VdRetrainEDRAM", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdRetrainEDRAMWorker)
{
    TraceImport("VdRetrainEDRAMWorker", ctx);
    ctx.r3.u64 = 0;
}

// ULONG VdInitializeEngines(ULONG, PVOID callback, ULONG, ULONG, ULONG)
PPC_FUNC(__imp__VdInitializeEngines)
{
    TraceImport("VdInitializeEngines", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__VdShutdownEngines)
{
    TraceImport("VdShutdownEngines", ctx);
}

// VOID VdSetGraphicsInterruptCallback(PVOID callback, PVOID context)
PPC_FUNC(__imp__VdSetGraphicsInterruptCallback)
{
    TraceImport("VdSetGraphicsInterruptCallback", ctx);

    g_video.interruptCallback = ctx.r3.u32;
    g_video.interruptContext = ctx.r4.u32;

    if (ctx.r3.u32)
        StartVblankThread();
}

// VOID VdCallGraphicsNotificationRoutines(ULONG source)
PPC_FUNC(__imp__VdCallGraphicsNotificationRoutines)
{
    TraceImport("VdCallGraphicsNotificationRoutines", ctx);
}

// VOID VdInitializeRingBuffer(PVOID base, ULONG sizeLog2)
PPC_FUNC(__imp__VdInitializeRingBuffer)
{
    TraceImport("VdInitializeRingBuffer", ctx);
    g_video.ringBase = ctx.r3.u32;
    g_video.ringSizeLog2 = ctx.r4.u32;
    GpuCpSetRing(ctx.r3.u32, ctx.r4.u32);
}

// VOID VdEnableRingBufferRPtrWriteBack(PVOID addr, ULONG blockSizeLog2)
PPC_FUNC(__imp__VdEnableRingBufferRPtrWriteBack)
{
    TraceImport("VdEnableRingBufferRPtrWriteBack", ctx);
    g_video.rptrWritebackAddr = ctx.r3.u32;
    g_video.rptrWritebackBlock = ctx.r4.u32;
    GpuCpSetWriteback(ctx.r3.u32);
    GpuCpStart();
}

// VOID VdSetSystemCommandBufferGpuIdentifierAddress(PVOID addr)
PPC_FUNC(__imp__VdSetSystemCommandBufferGpuIdentifierAddress)
{
    TraceImport("VdSetSystemCommandBufferGpuIdentifierAddress", ctx);
    g_video.gpuIdentifierAddr = ctx.r3.u32;
}

// VOID VdGetSystemCommandBuffer(PVOID* outBuffer, ULONG* outIdentifier)
PPC_FUNC(__imp__VdGetSystemCommandBuffer)
{
    TraceImport("VdGetSystemCommandBuffer", ctx);

    if (!g_video.systemCommandBuffer)
        g_video.systemCommandBuffer = GuestAlloc(GuestDomain::Physical, 0, 0x10000, 0x1000, true);

    if (ctx.r3.u32)
        GuestStore32(ctx.r3.u32, g_video.systemCommandBuffer);
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, 0xBEEF0000);
}

// ULONG VdInitializeScalerCommandBuffer(...)
PPC_FUNC(__imp__VdInitializeScalerCommandBuffer)
{
    TraceImport("VdInitializeScalerCommandBuffer", ctx);
    ctx.r3.u64 = 0;
}

// ULONG VdPersistDisplay(ULONG, PVOID outFlags)
PPC_FUNC(__imp__VdPersistDisplay)
{
    TraceImport("VdPersistDisplay", ctx);
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 1;
}

// VOID VdSwap(PVOID bufferPtr, PVOID fetchPtr, ...)
PPC_FUNC(__imp__VdSwap)
{
    const uint32_t n = ++g_video.swapCount;
    GpuCpFrameEnd();
    RendererSwap(ctx.r4.u32);
    if (n <= 3 || n % 600 == 0)
    {
        std::printf("[video] VdSwap #%u (buffer=0x%08X)\n", n, ctx.r3.u32);
        std::printf("[video]   r4=%08X r5=%08X r6=%08X r7=%08X fetch: %08X %08X %08X %08X %08X %08X\n", ctx.r4.u32,
                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, GuestLoad32(ctx.r4.u32), GuestLoad32(ctx.r4.u32 + 4),
                    GuestLoad32(ctx.r4.u32 + 8), GuestLoad32(ctx.r4.u32 + 12), GuestLoad32(ctx.r4.u32 + 16),
                    GuestLoad32(ctx.r4.u32 + 20));
        std::fflush(stdout);
    }
}

// The title's graphics interrupt handler is not reentrant: on the console interrupts are
// serialised. The vblank thread and the command processor both deliver interrupts, so they take
// this lock around the call.
static std::mutex g_interruptLock;

static void CallInterruptHandler(PPCContext& ctx, uint32_t source)
{
    const uint32_t cb = g_video.interruptCallback.load();
    if (!cb)
        return;

    PPCFunc* fn = LookupGuestFunction(cb);
    if (!fn)
        return;

    // Diagnostics: a source-1 interrupt is meant to find its callback pointer in scratch register
    // 4's mirrored word. Report the case where it is the cleared (poison) value.
    if (source == 1)
    {
        constexpr uint32_t kScratch4Mirror = 0xAAC00010;
        const uint32_t armed = GuestReadable(kScratch4Mirror, 4) ? GuestLoad32(kScratch4Mirror) : 0;
        // Poison (0x0BADF00D, also seen with its low half overwritten) or anything that is not the
        // entry of a recompiled function cannot be a callback the stream armed.
        const bool bogus = (armed >> 16) == 0x0BAD || (armed != 0 && !LookupGuestFunction(armed));
        if (GuestReadable(kScratch4Mirror, 4) && bogus)
        {
            static int reported = 0;
            if (reported++ < 5)
            {
                char where[160];
                GpuCpDescribeCurrent(where, sizeof(where));
                std::printf("[irq] source-1 interrupt with scratch4 cleared; packet: %s; reg 0x057C = 0x%08X\n", where,
                            GuestLoad32(guest::kGpuRegBase + 0x057C * 4));
                std::fflush(stdout);
                char scratch[256];
                GpuCpDescribeScratch(scratch, sizeof(scratch));
                std::printf("[irq] %s\n", scratch);
                GpuCpDumpAroundCurrent(20, 4);
                GpuCpDecodeBefore(40);
            }

            // The title would trap (twi 31) on this path, which the console never reaches: the
            // stream's own set of the callback slot was swallowed. Treat it as a spurious interrupt.
            return;
        }
    }

    std::lock_guard<std::mutex> lock(g_interruptLock);
    ctx.r3.u64 = source;
    ctx.r4.u64 = g_video.interruptContext.load();
    fn(ctx, g_guestBase);
}

// Delivers a graphics interrupt to the callback the title registered.
//
// This runs synchronously, in line with the PM4 packet that raised it. The title's command stream
// depends on that: it writes the callback pointer to a scratch register, raises the interrupt,
// then clears the register again and waits for the handler to acknowledge. Running the handler
// later would find the slot already cleared.
void VideoDispatchInterrupt(PPCContext& ctx, uint32_t source)
{
    CallInterruptHandler(ctx, source);
}

void VideoNotifySwap()
{
    ++g_video.swapCount;
}


// Debug: prints the ring-buffer bookkeeping and every non-zero word in the GPU register window.
void DebugDumpVideoState()
{
    std::printf("[video] ringBase=0x%08X log2=%u rptrWriteback=0x%08X block=%u gpuId=0x%08X sysCmd=0x%08X swaps=%u\n",
                g_video.ringBase, g_video.ringSizeLog2, g_video.rptrWritebackAddr, g_video.rptrWritebackBlock,
                g_video.gpuIdentifierAddr, g_video.systemCommandBuffer, g_video.swapCount.load());

    const GpuCpStats cp = GpuCpGetStats();
    std::printf("[gpu-cp] packets=%u type0=%u type3=%u ibs=%u memWrites=%u irq=%u swaps=%u unknown=%u\n", cp.packets, cp.type0,
                cp.type3, cp.indirectBuffers, cp.memoryWrites, cp.interrupts, cp.swaps, cp.unknownOpcodes);

    std::printf("[gpu-cp] state: %s, completed rptr=%u, current packet hdr=0x%08X opcode=0x%02X depth=%u\n",
                cp.busy ? "BUSY" : "idle", cp.rptr, cp.curHeader, cp.curOpcode, cp.curDepth);

    unsigned shown = 0;
    for (uint32_t off = 0; off < guest::kGpuRegSize && shown < 24; off += 4)
    {
        const uint32_t v = GuestLoad32(guest::kGpuRegBase + off);
        if (v != 0)
        {
            std::printf("[gpu-reg] +0x%05X (reg 0x%04X) = 0x%08X\n", off, off / 4, v);
            shown++;
        }
    }

    if (g_video.ringBase)
    {
        const uint32_t ring = guest::kPhysicalBegin + (g_video.ringBase & 0x1FFFFFFF);
        std::printf("[ring] @0x%08X:", ring);
        for (uint32_t i = 0; i < 24; i++)
            std::printf(" %08X", GuestLoad32(ring + i * 4));
        std::printf("\n");
    }

    if (g_video.rptrWritebackAddr)
    {
        const uint32_t blk = guest::kPhysicalBegin + ((g_video.rptrWritebackAddr - 0x3C) & 0x1FFFFFFF);
        std::printf("[gpublk] @0x%08X:", blk);
        for (uint32_t i = 0; i < 24; i++)
            std::printf(" %08X", GuestLoad32(blk + i * 4));
        std::printf("\n");
    }

    if (g_video.rptrWritebackAddr)
    {
        const uint32_t va = guest::kPhysicalBegin + (g_video.rptrWritebackAddr & 0x1FFFFFFF);
        std::printf("[video] *rptrWriteback (0x%08X) = 0x%08X\n", va, GuestLoad32(va));
    }
    std::fflush(stdout);
}
