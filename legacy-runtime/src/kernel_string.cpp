// xboxkrnl string/format/time helpers: Rtl*String, code page conversion, the CRT printf family and
// the TIME_FIELDS conversions.
#include "import_stubs.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

#include "kstatus.h"
#include "memory.h"

namespace
{
    // ---- varargs --------------------------------------------------------------------------------
    //
    // The Xbox 360 ABI numbers parameters as 8-byte "slots": slots 0..7 live in r3..r10 (integers)
    // or f1..f8 (floats), later slots are on the stack at r1 + 0x50 + (slot - 8) * 8.
    struct RegArgs
    {
        PPCContext& ctx;
        uint32_t slot;

        uint64_t Int()
        {
            const uint32_t s = slot++;
            switch (s)
            {
            case 0: return ctx.r3.u64;
            case 1: return ctx.r4.u64;
            case 2: return ctx.r5.u64;
            case 3: return ctx.r6.u64;
            case 4: return ctx.r7.u64;
            case 5: return ctx.r8.u64;
            case 6: return ctx.r9.u64;
            case 7: return ctx.r10.u64;
            default: return GuestLoad64(ctx.r1.u32 + 0x50 + (s - 8) * 8);
            }
        }

        double Double()
        {
            const uint32_t s = slot++;
            switch (s)
            {
            case 0: return ctx.f1.f64;
            case 1: return ctx.f2.f64;
            case 2: return ctx.f3.f64;
            case 3: return ctx.f4.f64;
            case 4: return ctx.f5.f64;
            case 5: return ctx.f6.f64;
            case 6: return ctx.f7.f64;
            case 7: return ctx.f8.f64;
            default:
            {
                const uint64_t bits = GuestLoad64(ctx.r1.u32 + 0x50 + (s - 8) * 8);
                double d;
                std::memcpy(&d, &bits, sizeof(d));
                return d;
            }
            }
        }
    };

    // va_list: pointer to consecutive 8-byte slots in guest memory.
    struct ListArgs
    {
        uint32_t ptr;

        uint64_t Int()
        {
            const uint64_t v = GuestLoad64(ptr);
            ptr += 8;
            return v;
        }

        double Double()
        {
            const uint64_t bits = Int();
            double d;
            std::memcpy(&d, &bits, sizeof(d));
            return d;
        }
    };

    std::string ReadCString(uint32_t ptr, size_t maxLen = 0x10000)
    {
        if (!ptr)
            return "(null)";
        const char* p = reinterpret_cast<const char*>(g_guestBase + ptr);
        size_t n = 0;
        while (n < maxLen && p[n])
            n++;
        return std::string(p, n);
    }

    std::string ReadWideString(uint32_t ptr)
    {
        if (!ptr)
            return "(null)";
        std::string s;
        for (uint32_t i = 0; i < 0x8000; i++)
        {
            const uint16_t c = GuestLoad16(ptr + i * 2);
            if (!c)
                break;
            s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        }
        return s;
    }

    template <typename Args>
    std::string Format(const std::string& fmt, Args& args)
    {
        std::string out;
        for (size_t i = 0; i < fmt.size(); i++)
        {
            const char c = fmt[i];
            if (c != '%')
            {
                out.push_back(c);
                continue;
            }

            if (++i >= fmt.size())
                break;
            if (fmt[i] == '%')
            {
                out.push_back('%');
                continue;
            }

            std::string flags;
            while (i < fmt.size() && std::strchr("-+ #0", fmt[i]))
                flags.push_back(fmt[i++]);

            std::string width;
            if (i < fmt.size() && fmt[i] == '*')
            {
                width = std::to_string(static_cast<int32_t>(args.Int()));
                i++;
            }
            else
            {
                while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
                    width.push_back(fmt[i++]);
            }

            std::string precision;
            bool hasPrecision = false;
            if (i < fmt.size() && fmt[i] == '.')
            {
                hasPrecision = true;
                i++;
                if (i < fmt.size() && fmt[i] == '*')
                {
                    precision = std::to_string(static_cast<int32_t>(args.Int()));
                    i++;
                }
                else
                {
                    while (i < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[i])))
                        precision.push_back(fmt[i++]);
                }
            }

            bool is64 = false, isShort = false, isWide = false;
            while (i < fmt.size())
            {
                if (fmt[i] == 'l')
                {
                    if (i + 1 < fmt.size() && fmt[i + 1] == 'l')
                    {
                        is64 = true;
                        i++;
                    }
                    isWide = true;
                    i++;
                }
                else if (fmt[i] == 'h')
                {
                    isShort = true;
                    i++;
                }
                else if (fmt[i] == 'I' && fmt.compare(i, 3, "I64") == 0)
                {
                    is64 = true;
                    i += 3;
                }
                else if (fmt[i] == 'w' || fmt[i] == 'z' || fmt[i] == 't' || fmt[i] == 'j' || fmt[i] == 'L')
                {
                    i++;
                }
                else
                {
                    break;
                }
            }

            if (i >= fmt.size())
                break;
            const char type = fmt[i];

            std::string spec = "%" + flags + width;
            if (hasPrecision)
                spec += "." + (precision.empty() ? std::string("0") : precision);

            char buf[512];
            switch (type)
            {
            case 'd':
            case 'i':
            {
                const uint64_t raw = args.Int();
                if (is64)
                    std::snprintf(buf, sizeof(buf), (spec + "lld").c_str(), static_cast<long long>(raw));
                else if (isShort)
                    std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int>(static_cast<int16_t>(raw)));
                else
                    std::snprintf(buf, sizeof(buf), (spec + "d").c_str(), static_cast<int>(static_cast<int32_t>(raw)));
                out += buf;
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                const uint64_t raw = args.Int();
                if (is64)
                    std::snprintf(buf, sizeof(buf), (spec + "ll" + type).c_str(), static_cast<unsigned long long>(raw));
                else if (isShort)
                    std::snprintf(buf, sizeof(buf), (spec + type).c_str(), static_cast<unsigned>(static_cast<uint16_t>(raw)));
                else
                    std::snprintf(buf, sizeof(buf), (spec + type).c_str(), static_cast<unsigned>(static_cast<uint32_t>(raw)));
                out += buf;
                break;
            }
            case 'p':
            {
                std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(args.Int()));
                out += buf;
                break;
            }
            case 'c':
            {
                const char ch = static_cast<char>(args.Int());
                std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), ch);
                out += buf;
                break;
            }
            case 's':
            case 'S':
            {
                const uint32_t p = static_cast<uint32_t>(args.Int());
                // %s is narrow unless "l"/"w" is given; %S is the opposite.
                const bool wide = (type == 's') ? isWide : !isWide;
                std::string s = wide ? ReadWideString(p) : ReadCString(p);
                if (hasPrecision && !precision.empty())
                    s = s.substr(0, static_cast<size_t>(std::stoul(precision)));

                size_t w = width.empty() ? 0 : static_cast<size_t>(std::atoi(width.c_str()));
                if (s.size() < w)
                {
                    const std::string pad(w - s.size(), ' ');
                    s = flags.find('-') != std::string::npos ? s + pad : pad + s;
                }
                out += s;
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                const double d = args.Double();
                std::snprintf(buf, sizeof(buf), (spec + type).c_str(), d);
                out += buf;
                break;
            }
            default:
                // Unknown conversion: emit it verbatim.
                out += '%';
                out += type;
                break;
            }
        }
        return out;
    }

    // Writes at most `limit` bytes (including the NUL) and returns the formatted length.
    uint32_t StoreFormatted(uint32_t dest, uint32_t limit, const std::string& s, bool* truncated = nullptr)
    {
        if (limit == 0 || !dest)
            return static_cast<uint32_t>(s.size());

        const size_t n = s.size() < limit - 1 ? s.size() : limit - 1;
        std::memcpy(g_guestBase + dest, s.data(), n);
        g_guestBase[dest + n] = 0;
        if (truncated)
            *truncated = s.size() > n;
        return static_cast<uint32_t>(s.size());
    }
}

// int sprintf(char* dest, const char* fmt, ...)
PPC_FUNC(__imp__sprintf)
{
    RegArgs args{ ctx, 2 };
    const std::string out = Format(ReadCString(ctx.r4.u32), args);
    StoreFormatted(ctx.r3.u32, 0x7FFFFFFF, out);
    ctx.r3.u64 = static_cast<uint64_t>(out.size());
}

// int _snprintf(char* dest, size_t count, const char* fmt, ...)
PPC_FUNC(__imp___snprintf)
{
    RegArgs args{ ctx, 3 };
    const uint32_t count = ctx.r4.u32;
    const std::string out = Format(ReadCString(ctx.r5.u32), args);

    bool truncated = false;
    StoreFormatted(ctx.r3.u32, count, out, &truncated);
    // MSVCRT semantics: -1 when the output did not fit (and no terminator is guaranteed).
    ctx.r3.u64 = truncated ? 0xFFFFFFFFFFFFFFFFull : static_cast<uint64_t>(out.size());
}

// int _vsnprintf(char* dest, size_t count, const char* fmt, va_list args)
PPC_FUNC(__imp___vsnprintf)
{
    ListArgs args{ ctx.r6.u32 };
    const uint32_t count = ctx.r4.u32;
    const std::string out = Format(ReadCString(ctx.r5.u32), args);

    bool truncated = false;
    StoreFormatted(ctx.r3.u32, count, out, &truncated);
    ctx.r3.u64 = truncated ? 0xFFFFFFFFFFFFFFFFull : static_cast<uint64_t>(out.size());
}

// ULONG DbgPrint(const char* fmt, ...)
// The title's own diagnostics: shown with a [guest] prefix, which is often the quickest way to
// learn why an engine decided to bail out.
PPC_FUNC(__imp__DbgPrint)
{
    RegArgs args{ ctx, 1 };
    std::string text = Format(ReadCString(ctx.r3.u32), args);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
        text.pop_back();

    std::printf("[guest] %s\n", text.c_str());
    std::fflush(stdout);
    ctx.r3.u64 = 0;
}

PPC_FUNC(__imp__DbgBreakPoint)
{
}

// VOID RtlInitAnsiString(PANSI_STRING dest, PCSZ src)
PPC_FUNC(__imp__RtlInitAnsiString)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t src = ctx.r4.u32;
    if (!dest)
        return;

    uint32_t len = 0;
    if (src)
    {
        const char* p = reinterpret_cast<const char*>(g_guestBase + src);
        while (len < 0xFFFE && p[len])
            len++;
    }

    GuestStore16(dest + 0, static_cast<uint16_t>(len));
    GuestStore16(dest + 2, static_cast<uint16_t>(src ? len + 1 : 0));
    GuestStore32(dest + 4, src);
}

// ULONG RtlNtStatusToDosError(NTSTATUS status)
PPC_FUNC(__imp__RtlNtStatusToDosError)
{
    switch (ctx.r3.u32)
    {
    case 0x00000000: ctx.r3.u64 = 0; break;                  // success
    case 0xC000000F:                                         // no such file
    case 0xC0000034: ctx.r3.u64 = 2; break;                  // object name not found
    case 0xC000003A: ctx.r3.u64 = 3; break;                  // path not found
    case 0xC0000022: ctx.r3.u64 = 5; break;                  // access denied
    case 0xC0000008: ctx.r3.u64 = 6; break;                  // invalid handle
    case 0xC0000017: ctx.r3.u64 = 8; break;                  // no memory
    case 0xC000000D: ctx.r3.u64 = 87; break;                 // invalid parameter
    case 0xC0000011: ctx.r3.u64 = 38; break;                 // end of file
    case 0xC0000035: ctx.r3.u64 = 183; break;                // name collision
    case 0x80000006: ctx.r3.u64 = 18; break;                 // no more files
    case 0x00000102: ctx.r3.u64 = 258; break;                // timeout
    default:         ctx.r3.u64 = 317; break;                // ERROR_MR_MID_NOT_FOUND
    }
}

// NTSTATUS RtlMultiByteToUnicodeN(PWCH dest, ULONG destMax, PULONG written, PCSZ src, ULONG srcLen)
PPC_FUNC(__imp__RtlMultiByteToUnicodeN)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t destMaxBytes = ctx.r4.u32;
    const uint32_t writtenPtr = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32;
    const uint32_t srcLen = ctx.r7.u32;

    uint32_t chars = srcLen;
    if (dest && chars > destMaxBytes / 2)
        chars = destMaxBytes / 2;

    for (uint32_t i = 0; dest && i < chars; i++)
        GuestStore16(dest + i * 2, g_guestBase[src + i]);

    if (writtenPtr)
        GuestStore32(writtenPtr, chars * 2);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS RtlUnicodeToMultiByteN(PCHAR dest, ULONG destMax, PULONG written, PCWCH src, ULONG srcBytes)
PPC_FUNC(__imp__RtlUnicodeToMultiByteN)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t destMax = ctx.r4.u32;
    const uint32_t writtenPtr = ctx.r5.u32;
    const uint32_t src = ctx.r6.u32;
    const uint32_t srcBytes = ctx.r7.u32;

    uint32_t chars = srcBytes / 2;
    if (dest && chars > destMax)
        chars = destMax;

    for (uint32_t i = 0; dest && i < chars; i++)
    {
        const uint16_t c = GuestLoad16(src + i * 2);
        g_guestBase[dest + i] = c < 0x100 ? static_cast<uint8_t>(c) : '?';
    }

    if (writtenPtr)
        GuestStore32(writtenPtr, chars);
    ctx.r3.u64 = nt::kSuccess;
}

// VOID RtlFillMemoryUlong(PVOID dest, SIZE_T length, ULONG pattern)
PPC_FUNC(__imp__RtlFillMemoryUlong)
{
    const uint32_t dest = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32 & ~3u;
    const uint32_t pattern = ctx.r5.u32;
    for (uint32_t i = 0; i < length; i += 4)
        GuestStore32(dest + i, pattern);
}

// SIZE_T RtlCompareMemoryUlong(PVOID src, SIZE_T length, ULONG pattern)
PPC_FUNC(__imp__RtlCompareMemoryUlong)
{
    const uint32_t src = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32 & ~3u;
    const uint32_t pattern = ctx.r5.u32;

    uint32_t i = 0;
    while (i < length && GuestLoad32(src + i) == pattern)
        i += 4;
    ctx.r3.u64 = i;
}

// VOID RtlTimeToTimeFields(LARGE_INTEGER* time, TIME_FIELDS* out)
PPC_FUNC(__imp__RtlTimeToTimeFields)
{
    const uint32_t timePtr = ctx.r3.u32;
    const uint32_t out = ctx.r4.u32;
    if (!timePtr || !out)
        return;

    const uint64_t t = GuestLoad64(timePtr);
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(t);
    ft.dwHighDateTime = static_cast<DWORD>(t >> 32);

    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);

    GuestStore16(out + 0, st.wYear);
    GuestStore16(out + 2, st.wMonth);
    GuestStore16(out + 4, st.wDay);
    GuestStore16(out + 6, st.wHour);
    GuestStore16(out + 8, st.wMinute);
    GuestStore16(out + 10, st.wSecond);
    GuestStore16(out + 12, st.wMilliseconds);
    GuestStore16(out + 14, st.wDayOfWeek);
}

// BOOLEAN RtlTimeFieldsToTime(TIME_FIELDS* fields, LARGE_INTEGER* out)
PPC_FUNC(__imp__RtlTimeFieldsToTime)
{
    const uint32_t in = ctx.r3.u32;
    const uint32_t outPtr = ctx.r4.u32;
    if (!in || !outPtr)
    {
        ctx.r3.u64 = 0;
        return;
    }

    SYSTEMTIME st{};
    st.wYear = GuestLoad16(in + 0);
    st.wMonth = GuestLoad16(in + 2);
    st.wDay = GuestLoad16(in + 4);
    st.wHour = GuestLoad16(in + 6);
    st.wMinute = GuestLoad16(in + 8);
    st.wSecond = GuestLoad16(in + 10);
    st.wMilliseconds = GuestLoad16(in + 12);

    FILETIME ft;
    if (!SystemTimeToFileTime(&st, &ft))
    {
        ctx.r3.u64 = 0;
        return;
    }

    GuestStore64(outPtr, (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime);
    ctx.r3.u64 = 1;
}
