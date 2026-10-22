// System configuration, XAM user/notification/UI exports. There is no signed-in profile, no
// Live connection and no controller front end yet, so these report "nobody is here" in the way
// the real services do.
#include "import_stubs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <mutex>
#include <map>
#include <deque>

#include "guest_alloc.h"
#include "kobject.h"
#include "kstatus.h"
#include "memory.h"
#include "renderer.h"
#include "xex_headers.h"

namespace
{
    constexpr uint32_t kErrorAccessDenied = 5;
    constexpr uint32_t kErrorNotSupported = 0x32;
    constexpr uint32_t kErrorDeviceNotConnected = 0x48F;
    constexpr uint32_t kErrorNoSuchUser = 0x525;

    constexpr uint32_t kStatusBufferTooSmall = 0xC0000023;
    constexpr uint32_t kStatusInvalidParameter2 = 0xC00000F0;
    constexpr uint32_t kStatusInvalidParameter1 = 0xC00000EF;

    constexpr uint32_t kXamNotifyUiPosition = 0;

    // Returns true and sets `value` if the user-category setting is known.
    bool UserSetting(uint32_t setting, uint32_t& value)
    {
        switch (setting)
        {
        case 0x0001: value = 0;          return true; // time zone bias
        case 0x0006: value = 0;          return true; // standard bias
        case 0x0007: value = 0;          return true; // daylight bias
        case 0x0009: value = 1;          return true; // language (1 = English)
        case 0x000A: value = 0x00040000; return true; // video flags (widescreen)
        case 0x000B: value = 0x00010001; return true; // audio flags
        case 0x000C: value = 0x00000040; return true; // retail flags
        case 0x000E: value = 103;        return true; // country
        case 0x000F: value = 0;          return true; // parental control flags
        default:                         return false;
        }
    }
}

// NTSTATUS ExGetXConfigSetting(WORD category, WORD setting, PVOID buffer, WORD bufferSize, WORD* requiredSize)
PPC_FUNC(__imp__ExGetXConfigSetting)
{
    TraceImport("ExGetXConfigSetting", ctx);

    const uint32_t category = ctx.r3.u32 & 0xFFFF;
    const uint32_t setting = ctx.r4.u32 & 0xFFFF;
    const uint32_t buffer = ctx.r5.u32;
    const uint32_t bufferSize = ctx.r6.u32 & 0xFFFF;
    const uint32_t requiredPtr = ctx.r7.u32;

    uint32_t value = 0;
    constexpr uint32_t kUserCategory = 3;

    if (category != kUserCategory)
    {
        ctx.r3.u64 = kStatusInvalidParameter1;
        return;
    }

    if (!UserSetting(setting, value))
    {
        ctx.r3.u64 = kStatusInvalidParameter2;
        return;
    }

    if (requiredPtr)
        GuestStore16(requiredPtr, 4);

    if (!buffer || bufferSize < 4)
    {
        ctx.r3.u64 = kStatusBufferTooSmall;
        return;
    }

    GuestStore32(buffer, value);
    ctx.r3.u64 = nt::kSuccess;
}

// BOOLEAN XexCheckExecutablePrivilege(ULONG privilege)
PPC_FUNC(__imp__XexCheckExecutablePrivilege)
{
    TraceImport("XexCheckExecutablePrivilege", ctx);
    ctx.r3.u64 = 0;
}

// ULONG XGetLanguage()
PPC_FUNC(__imp__XGetLanguage)
{
    ctx.r3.u64 = 1; // English
}

// ULONG XGetGameRegion()
PPC_FUNC(__imp__XGetGameRegion)
{
    ctx.r3.u64 = 0xFFFFFFFF; // region free
}

// ULONG XamGetExecutionId(XEX_EXECUTION_ID** out)
PPC_FUNC(__imp__XamGetExecutionId)
{
    TraceImport("XamGetExecutionId", ctx);

    constexpr uint32_t kExecutionInfoKey = 0x00040006;
    const uint32_t info = XexHeaderFieldGuest(kExecutionInfoKey);
    if (!info || !ctx.r3.u32)
    {
        ctx.r3.u64 = nt::kNotFound;
        return;
    }

    GuestStore32(ctx.r3.u32, info);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamLoaderTerminateTitle)
{
    std::printf("[xam] XamLoaderTerminateTitle\n");
    std::fflush(stdout);
    std::exit(0);
}

// ---- memory ---------------------------------------------------------------------------------

// ULONG XamAlloc(ULONG flags, ULONG size, PVOID* out)
PPC_FUNC(__imp__XamAlloc)
{
    const uint32_t size = ctx.r4.u32;
    const uint32_t out = ctx.r5.u32;

    const uint32_t addr = GuestAlloc(GuestDomain::User, 0, size ? size : 1, 0x1000, true);
    if (!addr || !out)
    {
        ctx.r3.u64 = nt::kNoMemory;
        return;
    }

    GuestStore32(out, addr);
    ctx.r3.u64 = 0;
}

// VOID XamFree(PVOID ptr)
PPC_FUNC(__imp__XamFree)
{
    GuestRelease(ctx.r3.u32, nullptr);
    ctx.r3.u64 = 0;
}

// ---- users / profile ------------------------------------------------------------------------

// ULONG XamUserGetSigninState(ULONG userIndex)  -> 0 = not signed in, 1 = signed in locally
PPC_FUNC(__imp__XamUserGetSigninState)
{
    TraceImport("XamUserGetSigninState", ctx);
    const uint32_t user = ctx.r3.u32;
    static const uint32_t state = [] {
        const char* e = std::getenv("Q4_SIGNIN");
        return e ? static_cast<uint32_t>(std::atoi(e)) : 1u;
    }();
    ctx.r3.u64 = (user == 0 || user == 0xFF || user == 0xFFFFFFFF) ? state : 0;
}

// ULONG XamUserGetXUID(ULONG user, ULONG flags, XUID* out)
PPC_FUNC(__imp__XamUserGetXUID)
{
    TraceImport("XamUserGetXUID", ctx);
    const uint32_t user = ctx.r3.u32;
    {
        // Diagnostics: GetXUID calls made on behalf of the Live party code.
        static unsigned logged = 0;
        const uint32_t lr = static_cast<uint32_t>(ctx.lr);
        if (logged < 20 && std::getenv("Q4_XUIDLOG"))
        {
            logged++;
            std::printf("[xuid] user=0x%08X flags=0x%08X out=0x%08X lr=0x%08X\n", user, ctx.r4.u32, ctx.r5.u32, lr);
            std::fflush(stdout);
        }
    }
    // XUSER_INDEX_ANY is not a user: the title asks with ANY until the player has chosen a
    // controller, and relies on this failing (it then keeps a zero XUID everywhere).
    if (user != 0 && user != 0xFF)
    {
        if (ctx.r5.u32)
        {
            GuestStore32(ctx.r5.u32 + 0, 0); // the output is zeroed on failure
            GuestStore32(ctx.r5.u32 + 4, 0);
        }
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }

    if (ctx.r5.u32)
    {
        static const uint32_t hi = [] {
            const char* e = std::getenv("Q4_XUID_HI");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0xE0000000u;
        }();
        GuestStore32(ctx.r5.u32 + 0, hi); // offline XUID prefix by default
        GuestStore32(ctx.r5.u32 + 4, 0x00000001);
    }
    ctx.r3.u64 = 0;
}

// ULONG XamUserGetName(ULONG user, CHAR* buffer, ULONG size)
PPC_FUNC(__imp__XamUserGetName)
{
    TraceImport("XamUserGetName", ctx);
    const uint32_t user = ctx.r3.u32;
    if (user != 0 && user != 0xFF && user != 0xFFFFFFFF)
    {
        ctx.r3.u64 = kErrorNoSuchUser;
        return;
    }

    static const char kName[] = "Player";
    if (ctx.r4.u32 && ctx.r5.u32 >= sizeof(kName))
        std::memcpy(g_guestBase + ctx.r4.u32, kName, sizeof(kName));
    ctx.r3.u64 = 0;
}

// ULONG XamUserCheckPrivilege(ULONG user, ULONG privilege, BOOLEAN* out)
PPC_FUNC(__imp__XamUserCheckPrivilege)
{
    if (ctx.r5.u32)
        g_guestBase[ctx.r5.u32] = 0;
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XamUserReadProfileSettings)
{
    TraceImport("XamUserReadProfileSettings", ctx);

    // (titleId, userIndex, xuidCount, xuids, settingCount, settingIds, *bufferSize, results, overlapped)
    // Every requested setting is reported with its default (int32 0) value.
    const uint32_t count = ctx.r7.u32;
    const uint32_t ids = ctx.r8.u32;
    const uint32_t sizePtr = ctx.r9.u32;
    const uint32_t results = ctx.r10.u32;
    const uint32_t overlapped = GuestLoad32(ctx.r1.u32 + 0x54);

    constexpr uint32_t kEntrySize = 40; // XUSER_PROFILE_SETTING
    const uint32_t needed = 8 + count * kEntrySize;

    uint32_t status = 0;
    if (!sizePtr)
    {
        status = 87; // ERROR_INVALID_PARAMETER
    }
    else if (!results || GuestLoad32(sizePtr) < needed)
    {
        GuestStore32(sizePtr, needed);
        status = 122; // ERROR_INSUFFICIENT_BUFFER
    }
    else
    {
        std::memset(g_guestBase + results, 0, needed);
        GuestStore32(results + 0, count);
        GuestStore32(results + 4, count ? results + 8 : 0);
        for (uint32_t i = 0; i < count; i++)
        {
            const uint32_t e = results + 8 + i * kEntrySize;
            GuestStore32(e + 0, 1);                         // source: default
            GuestStore32(e + 16, GuestLoad32(ids + i * 4)); // setting id
            g_guestBase[e + 24] = 1;                        // XUSER_DATA_TYPE_INT32
        }
        status = 0;
    }

    if (overlapped)
    {
        GuestStore32(overlapped + 0x18, status);
        GuestStore32(overlapped + 0x04, 0);
        GuestStore32(overlapped + 0x00, status);
        const uint32_t ev = GuestLoad32(overlapped + 0x0C);
        if (ev)
        {
            if (auto obj = HandleGet(ev); obj && obj->host)
                SetEvent(obj->host);
        }
        ctx.r3.u64 = 0x3E5; // ERROR_IO_PENDING; completion already recorded
        return;
    }

    ctx.r3.u64 = status;
}

PPC_FUNC(__imp__XamUserWriteProfileSettings)
{
    ctx.r3.u64 = kErrorNoSuchUser;
}

PPC_FUNC(__imp__XamUserAreUsersFriends)
{
    ctx.r3.u64 = kErrorNoSuchUser;
}

PPC_FUNC(__imp__XamUserCreateStatsEnumerator)
{
    ctx.r3.u64 = kErrorNoSuchUser;
}

// ---- notifications --------------------------------------------------------------------------

// Notification queues, one per listener handle. A listener starts with the notifications a console
// delivers at boot (the signed-in profile), so the title learns who is signed in.
namespace
{
    constexpr uint32_t kXnSysSigninChanged = 0x0000000A;
    constexpr uint32_t kXnSysProfileSettingChanged = 0x0000000E;
    constexpr uint32_t kXnSysInputDevicesChanged = 0x00000012;

    std::mutex g_notifyMutex;
    std::map<uint32_t, std::deque<std::pair<uint32_t, uint32_t>>> g_notifyQueues;
}

// HANDLE XamNotifyCreateListener(ULONGLONG mask, ULONG maxVersion)
PPC_FUNC(__imp__XamNotifyCreateListener)
{
    TraceImport("XamNotifyCreateListener", ctx);

    auto obj = std::make_shared<KObject>(KType::Event);
    obj->manualReset = true;
    obj->host = CreateEventW(nullptr, TRUE, TRUE, nullptr); // signalled: notifications pending
    HANDLE hostEvent = obj->host;
    const uint32_t handle = HandleCreate(std::move(obj));

    {
        std::lock_guard<std::mutex> lock(g_notifyMutex);
        auto& q = g_notifyQueues[handle];
        q.push_back({ kXnSysSigninChanged, 1 });          // user 0 signed in
        q.push_back({ kXnSysProfileSettingChanged, 1 });
        q.push_back({ kXnSysInputDevicesChanged, 1 });
    }
    (void)hostEvent;
    ctx.r3.u64 = handle;
}

// BOOLEAN XNotifyGetNext(HANDLE listener, ULONG id, ULONG* outId, ULONG* outParam)
PPC_FUNC(__imp__XNotifyGetNext)
{
    const uint32_t handle = ctx.r3.u32;
    const uint32_t filter = ctx.r4.u32;
    const uint32_t outId = ctx.r5.u32;
    const uint32_t outParam = ctx.r6.u32;

    std::lock_guard<std::mutex> lock(g_notifyMutex);
    auto it = g_notifyQueues.find(handle);
    if (it != g_notifyQueues.end())
    {
        auto& q = it->second;
        for (auto n = q.begin(); n != q.end(); ++n)
        {
            if (filter != 0 && n->first != filter)
                continue;

            if (outId)
                GuestStore32(outId, n->first);
            if (outParam)
                GuestStore32(outParam, n->second);
            TraceImport("XNotifyGetNext", ctx);
            q.erase(n);
            if (q.empty())
            {
                if (auto obj = HandleGet(handle); obj && obj->host)
                    ResetEvent(obj->host);
            }
            ctx.r3.u64 = 1;
            return;
        }

        if (q.empty())
        {
            if (auto obj = HandleGet(handle); obj && obj->host)
                ResetEvent(obj->host);
        }
    }

    if (outId)
        GuestStore32(outId, 0);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XNotifyPositionUI)
{
    ctx.r3.u64 = kXamNotifyUiPosition;
}

// ---- UI ---------------------------------------------------------------------------------------
// There is no dashboard overlay: every request fails immediately so the title never waits on it.

#define Q4_XAM_UI_STUB(name)                 \
    PPC_FUNC(__imp__##name)                  \
    {                                        \
        TraceImport(#name, ctx);             \
        ctx.r3.u64 = kErrorAccessDenied;     \
    }

Q4_XAM_UI_STUB(XamShowSigninUI)
Q4_XAM_UI_STUB(XamShowFriendsUI)
Q4_XAM_UI_STUB(XamShowGamerCardUIForXUID)
Q4_XAM_UI_STUB(XamShowAchievementsUI)
Q4_XAM_UI_STUB(XamShowPlayerReviewUI)
Q4_XAM_UI_STUB(XamShowMarketplaceUI)
Q4_XAM_UI_STUB(XamShowDeviceSelectorUI)
Q4_XAM_UI_STUB(XamShowMessageBoxUIEx)

// ---- messaging / input ------------------------------------------------------------------------

PPC_FUNC(__imp__XMsgInProcessCall)
{
    TraceImport("XMsgInProcessCall", ctx);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMsgStartIORequest)
{
    TraceImport("XMsgStartIORequest", ctx);

    // (app, message, overlapped, buffer, length). XGI (app 0xFB) messages are context/property
    // updates and session calls; accept them all. An overlapped is completed immediately.
    const uint32_t overlapped = ctx.r5.u32;
    if (overlapped)
    {
        GuestStore32(overlapped + 0x18, 0);
        GuestStore32(overlapped + 0x04, 0);
        GuestStore32(overlapped + 0x00, 0);
        const uint32_t ev = GuestLoad32(overlapped + 0x0C);
        if (ev)
        {
            if (auto obj = HandleGet(ev); obj && obj->host)
                SetEvent(obj->host);
        }
    }
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__XMsgCancelIORequest)
{
    ctx.r3.u64 = 0;
}

// One virtual controller (user 0) with no buttons pressed.
namespace
{
    bool ValidPad(uint32_t user)
    {
        return user == 0 || user == 0xFF || user == 0xFFFFFFFF;
    }
}

// DWORD XamInputGetCapabilities(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps)
PPC_FUNC(__imp__XamInputGetCapabilities)
{
    TraceImport("XamInputGetCapabilities", ctx);
    if (!ValidPad(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }

    if (ctx.r5.u32)
    {
        std::memset(g_guestBase + ctx.r5.u32, 0, 20);
        g_guestBase[ctx.r5.u32 + 0] = 1; // XINPUT_DEVTYPE_GAMEPAD
        g_guestBase[ctx.r5.u32 + 1] = 1; // XINPUT_DEVSUBTYPE_GAMEPAD
        GuestStore16(ctx.r5.u32 + 4, 0xF3FF); // buttons supported
        g_guestBase[ctx.r5.u32 + 6] = 0xFF;   // triggers
        g_guestBase[ctx.r5.u32 + 7] = 0xFF;
        GuestStore16(ctx.r5.u32 + 8, 0x7FFF);   // thumbs
        GuestStore16(ctx.r5.u32 + 10, 0x7FFF);
        GuestStore16(ctx.r5.u32 + 12, 0x7FFF);
        GuestStore16(ctx.r5.u32 + 14, 0x7FFF);
        GuestStore16(ctx.r5.u32 + 16, 0xFFFF); // vibration
        GuestStore16(ctx.r5.u32 + 18, 0xFFFF);
    }
    ctx.r3.u64 = 0;
}

// DWORD XamInputGetState(DWORD user, DWORD flags, XINPUT_STATE* state)
PPC_FUNC(__imp__XamInputGetState)
{
    TraceImport("XamInputGetState", ctx);
    if (!ValidPad(ctx.r3.u32))
    {
        ctx.r3.u64 = kErrorDeviceNotConnected;
        return;
    }

    if (ctx.r5.u32)
    {
        static uint32_t packet = 0;
        std::memset(g_guestBase + ctx.r5.u32, 0, 16);

        // Q4_AUTOBTN=<hex mask>: tap that button for 150 ms every second after 8 s (headless
        // progression through "press start" screens).
        static const uint32_t autoMask = [] {
            const char* e = std::getenv("Q4_AUTOBTN");
            return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
        }();
        static const uint64_t t0 = GetTickCount64();
        const uint64_t t = GetTickCount64() - t0;
        static const bool hold = std::getenv("Q4_AUTOHOLD") != nullptr;
        const uint32_t buttons = (autoMask && t > 8000 && (hold || (t % 1000) < 150)) ? autoMask : 0;

        GuestStore32(ctx.r5.u32, ++packet);
        const HostPad host = RendererPollInput();
        GuestStore16(ctx.r5.u32 + 4, static_cast<uint16_t>(buttons | host.buttons));
        g_guestBase[ctx.r5.u32 + 6] = host.leftTrigger;
        g_guestBase[ctx.r5.u32 + 7] = host.rightTrigger;
        for (int k = 0; k < 4; k++)
            if (host.thumb[k])
                GuestStore16(ctx.r5.u32 + 8 + 2 * k, static_cast<uint16_t>(host.thumb[k]));
        {
            // Q4_STICK="lx,ly,rx,ry": constant stick deflection (int16) applied after 8 s, for headless camera/movement tests.
            static int st[4] = {};
            static const bool have = [] {
                const char* e = std::getenv("Q4_STICK");
                return e && std::sscanf(e, "%d,%d,%d,%d", &st[0], &st[1], &st[2], &st[3]) == 4;
            }();
            for (int k = 0; k < 4; k++)
                if (have && t > 8000 && !host.thumb[k])
                    GuestStore16(ctx.r5.u32 + 8 + 2 * k, static_cast<uint16_t>(st[k]));
        }
    }
    ctx.r3.u64 = 0;
}

// DWORD XamInputSetState(DWORD user, DWORD flags, XINPUT_VIBRATION* vibration)
PPC_FUNC(__imp__XamInputSetState)
{
    TraceImport("XamInputSetState", ctx);
    ctx.r3.u64 = ValidPad(ctx.r3.u32) ? 0 : kErrorDeviceNotConnected;
}

// BOOLEAN XamVoiceHeadsetPresent(...)
PPC_FUNC(__imp__XamVoiceHeadsetPresent)
{
    ctx.r3.u64 = 0;
}
