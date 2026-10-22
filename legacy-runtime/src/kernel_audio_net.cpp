// XAudio render-driver client and NetDll stubs.
//
// There is no audio output yet, but the title's mixer thread is driven by the render-driver
// callback: it blocks until the system asks for the next frame. A ticker therefore calls the
// registered callback at the hardware cadence (256 samples at 48 kHz). Networking reports "no
// link" so the title falls back to offline operation.
#include "import_stubs.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "memory.h"
#include "thread.h"

namespace
{
    constexpr uint32_t kDriverHandle = 0x41550001; // 'AU' + 1
    std::atomic<uint32_t> g_callback{ 0 };
    std::atomic<uint32_t> g_context{ 0 };
    std::atomic<bool> g_ticker{ false };

    void StartAudioTicker()
    {
        bool expected = false;
        if (!g_ticker.compare_exchange_strong(expected, true))
            return;

        CreateGuestWorker([](PPCContext& ctx)
        {
            using clock = std::chrono::steady_clock;
            auto next = clock::now();
            for (;;)
            {
                next += std::chrono::microseconds(5333);
                std::this_thread::sleep_until(next);

                const uint32_t cb = g_callback.load();
                if (!cb)
                    continue;

                if (PPCFunc* fn = LookupGuestFunction(cb))
                {
                    ctx.r3.u64 = g_context.load();
                    fn(ctx, g_guestBase);
                }
            }
        });
    }
}

// DWORD XAudioGetSpeakerConfig(DWORD* config)
PPC_FUNC(__imp__XAudioGetSpeakerConfig)
{
    TraceImport("XAudioGetSpeakerConfig", ctx);
    if (ctx.r3.u32)
        GuestStore32(ctx.r3.u32, 1); // stereo
    ctx.r3.u64 = 0;
}

// HRESULT XAudioRegisterRenderDriverClient(XAUDIO_RENDER_DRIVER_CLIENT* client, DWORD* driver)
PPC_FUNC(__imp__XAudioRegisterRenderDriverClient)
{
    TraceImport("XAudioRegisterRenderDriverClient", ctx);
    const uint32_t client = ctx.r3.u32;
    if (client)
    {
        g_callback = GuestLoad32(client + 0);
        g_context = GuestLoad32(client + 4);
    }
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, kDriverHandle);
    StartAudioTicker();
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioUnregisterRenderDriverClient)
{
    g_callback = 0;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioSubmitRenderDriverFrame)
{
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolume)
{
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, 0x3F800000); // 1.0f
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XAudioGetVoiceCategoryVolumeChangeMask)
{
    if (ctx.r4.u32)
        GuestStore32(ctx.r4.u32, 0);
    ctx.r3.u64 = 0;
}

// ---- networking: offline ---------------------------------------------------------------------

#define Q4_NET_OK(name) PPC_FUNC(__imp__##name) { TraceImport(#name, ctx); ctx.r3.u64 = 0; }
#define Q4_NET_FAIL(name) PPC_FUNC(__imp__##name) { TraceImport(#name, ctx); ctx.r3.u64 = 0xFFFFFFFF; }

Q4_NET_OK(NetDll_XNetStartup)
// int NetDll_WSAStartup(XNCALLER, WORD version, WSADATA* data)
PPC_FUNC(__imp__NetDll_WSAStartup)
{
    TraceImport("NetDll_WSAStartup", ctx);
    if (ctx.r5.u32)
    {
        GuestStore16(ctx.r5.u32 + 0, 0x0202); // wVersion
        GuestStore16(ctx.r5.u32 + 2, 0x0202); // wHighVersion
    }
    ctx.r3.u64 = 0;
}
Q4_NET_OK(NetDll_WSACleanup)
Q4_NET_OK(NetDll_XNetRegisterKey)
Q4_NET_OK(NetDll_XNetGetEthernetLinkStatus) // 0 = no link
Q4_NET_FAIL(NetDll_socket)
Q4_NET_OK(NetDll_closesocket)
Q4_NET_FAIL(NetDll_ioctlsocket)
Q4_NET_FAIL(NetDll_setsockopt)
Q4_NET_FAIL(NetDll_getsockname)
Q4_NET_FAIL(NetDll_bind)
Q4_NET_FAIL(NetDll_select)
Q4_NET_FAIL(NetDll_recvfrom)
Q4_NET_FAIL(NetDll_sendto)
Q4_NET_OK(NetDll_inet_addr)

PPC_FUNC(__imp__NetDll_WSAGetLastError)
{
    ctx.r3.u64 = 10050; // WSAENETDOWN
}

PPC_FUNC(__imp__NetDll_XNetGetTitleXnAddr)
{
    ctx.r3.u64 = 0x2; // XNET_GET_XNADDR_NONE: offline
}

// ---- XMA hardware decoder: contexts exist but never decode ---------------------------------------
// The title feeds compressed buffers to numbered XMA contexts and polls the "buffer valid" flags
// to learn when they were consumed. With nothing decoding, every buffer reads as already consumed.
#include "imports.h"

PPC_FUNC(__imp__XMACreateContext)
{
    TraceImport("XMACreateContext", ctx);
    static std::atomic<uint32_t> next{ 0 };
    const uint32_t out = ctx.r3.u32;
    if (out)
    {
        const uint32_t blk = KernelAlloc(0x40);
        GuestStore32(out, blk ? blk : 0x7FF00000u + (next++) * 0x40);
    }
    ctx.r3.u64 = 0;
}

#define Q4_XMA_ZERO(name) PPC_FUNC(__imp__##name) { ctx.r3.u64 = 0; }

Q4_XMA_ZERO(XMAReleaseContext)
Q4_XMA_ZERO(XMAInitializeContext)
Q4_XMA_ZERO(XMAEnableContext)
Q4_XMA_ZERO(XMADisableContext)
Q4_XMA_ZERO(XMABlockWhileInUse)
Q4_XMA_ZERO(XMAIsInputBuffer0Valid)
Q4_XMA_ZERO(XMAIsInputBuffer1Valid)
Q4_XMA_ZERO(XMAIsOutputBufferValid)
Q4_XMA_ZERO(XMASetInputBuffer0)
Q4_XMA_ZERO(XMASetInputBuffer1)
Q4_XMA_ZERO(XMASetInputBuffer0Valid)
Q4_XMA_ZERO(XMASetInputBuffer1Valid)
Q4_XMA_ZERO(XMASetInputBufferReadOffset)
Q4_XMA_ZERO(XMASetLoopData)
Q4_XMA_ZERO(XMASetOutputBufferReadOffset)
Q4_XMA_ZERO(XMASetOutputBufferValid)
Q4_XMA_ZERO(XMAGetOutputBufferReadOffset)
Q4_XMA_ZERO(XMAGetOutputBufferWriteOffset)
