// XAM content (save game / marketplace) enumeration and the XOVERLAPPED completion protocol.
//
// There is no storage device model yet: enumerators exist but are empty, devices report "not
// connected" and content creation fails. What matters is that every asynchronous call completes
// its XOVERLAPPED, because the title blocks on it.
#include "import_stubs.h"

#include <cstdio>
#include <memory>

#include "kobject.h"
#include "kstatus.h"
#include "memory.h"

namespace
{
    constexpr uint32_t kErrorSuccess = 0;
    constexpr uint32_t kErrorNoMoreFiles = 18;
    constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
    constexpr uint32_t kErrorFunctionFailed = 0x1F;
    constexpr uint32_t kErrorIoPending = 0x3E5;
    constexpr uint32_t kHresultNoMoreFiles = 0x80070012;

    constexpr uint32_t kContentDataSize = 0x134; // XCONTENT_DATA

    // XOVERLAPPED layout (guest, big endian):
    //   +0x00 result / internal low     +0x04 length / internal high   +0x08 context
    //   +0x0C event handle              +0x10 completion routine       +0x14 completion context
    //   +0x18 extended error
    void CompleteOverlapped(uint32_t ptr, uint32_t result, uint32_t extendedError, uint32_t length)
    {
        if (!ptr)
            return;

        GuestStore32(ptr + 0x18, extendedError);
        GuestStore32(ptr + 0x04, length);
        GuestStore32(ptr + 0x00, result);

        const uint32_t eventHandle = GuestLoad32(ptr + 0x0C);
        if (eventHandle)
        {
            if (auto ev = HandleGet(eventHandle); ev && ev->host)
                SetEvent(ev->host);
        }
    }
}

// DWORD XamContentCreateEnumerator(DWORD user, DWORD device, DWORD contentType, DWORD flags,
//                                  DWORD itemsPerEnumerate, DWORD* bufferSize, HANDLE* handle)
PPC_FUNC(__imp__XamContentCreateEnumerator)
{
    TraceImport("XamContentCreateEnumerator", ctx);

    const uint32_t itemsPerEnumerate = ctx.r7.u32;
    const uint32_t bufferSizePtr = ctx.r8.u32;
    const uint32_t handlePtr = ctx.r9.u32;

    if (bufferSizePtr)
        GuestStore32(bufferSizePtr, itemsPerEnumerate * kContentDataSize);

    // The enumerator is only a handle the title can pass to XamEnumerate and close again.
    auto obj = std::make_shared<KObject>(KType::Event);
    obj->manualReset = true;
    obj->host = CreateEventW(nullptr, TRUE, TRUE, nullptr);

    const uint32_t handle = HandleCreate(std::move(obj));
    if (handlePtr)
        GuestStore32(handlePtr, handle);

    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamEnumerate(HANDLE enumerator, DWORD flags, PVOID buffer, DWORD bufferSize,
//                    DWORD* itemsReturned, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamEnumerate)
{
    TraceImport("XamEnumerate", ctx);

    const uint32_t itemsPtr = ctx.r7.u32;
    const uint32_t overlapped = ctx.r8.u32;

    // The enumerator is empty.
    if (itemsPtr)
        GuestStore32(itemsPtr, 0);

    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorNoMoreFiles, kHresultNoMoreFiles, 0);
        ctx.r3.u64 = kErrorIoPending; // the title then reads the already-completed structure
        return;
    }

    ctx.r3.u64 = kErrorNoMoreFiles;
}

// DWORD XamContentGetDeviceState(DWORD device, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentGetDeviceState)
{
    TraceImport("XamContentGetDeviceState", ctx);

    const uint32_t overlapped = ctx.r4.u32;
    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorDeviceNotConnected, kErrorDeviceNotConnected, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }

    ctx.r3.u64 = kErrorDeviceNotConnected;
}

// DWORD XamContentGetDeviceData(DWORD device, XDEVICE_DATA* out)
PPC_FUNC(__imp__XamContentGetDeviceData)
{
    TraceImport("XamContentGetDeviceData", ctx);
    ctx.r3.u64 = kErrorDeviceNotConnected;
}

// DWORD XamContentCreate(DWORD user, const char* root, XCONTENT_DATA* data, DWORD flags,
//                        DWORD* disposition, DWORD* license, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentCreate)
{
    TraceImport("XamContentCreate", ctx);

    const uint32_t dispositionPtr = ctx.r7.u32;
    const uint32_t overlapped = ctx.r9.u32;

    if (dispositionPtr)
        GuestStore32(dispositionPtr, 0);

    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorFunctionFailed, kErrorFunctionFailed, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }

    ctx.r3.u64 = kErrorFunctionFailed;
}

// DWORD XamContentClose(const char* root, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentClose)
{
    TraceImport("XamContentClose", ctx);

    const uint32_t overlapped = ctx.r4.u32;
    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorSuccess, kErrorSuccess, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }

    ctx.r3.u64 = kErrorSuccess;
}

// DWORD XamContentDelete(DWORD user, XCONTENT_DATA* data, XOVERLAPPED* overlapped)
PPC_FUNC(__imp__XamContentDelete)
{
    TraceImport("XamContentDelete", ctx);

    const uint32_t overlapped = ctx.r5.u32;
    if (overlapped)
    {
        CompleteOverlapped(overlapped, kErrorFunctionFailed, kErrorFunctionFailed, 0);
        ctx.r3.u64 = kErrorIoPending;
        return;
    }

    ctx.r3.u64 = kErrorFunctionFailed;
}
