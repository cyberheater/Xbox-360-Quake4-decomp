// xboxkrnl file-system exports (Nt*File). Guest paths are mapped through vfs.h onto the extracted
// game directory (read-only) and a writable save/cache directory.
#include "import_stubs.h"

#include <cstdio>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <windows.h>

#include "backtrace.h"
#include "kobject.h"
#include "kstatus.h"
#include "memory.h"
#include "vfs.h"

namespace
{
    // ---- status codes specific to the file system ----------------------------------------------
    constexpr uint32_t kStatusInfoLengthMismatch = 0xC0000004;
    constexpr uint32_t kStatusObjectNameCollision = 0xC0000035;
    constexpr uint32_t kStatusObjectPathNotFound = 0xC000003A;
    constexpr uint32_t kStatusNotADirectory = 0xC0000103;
    constexpr uint32_t kStatusFileIsADirectory = 0xC00000BA;
    constexpr uint32_t kStatusAccessDenied = 0xC0000022;
    constexpr uint32_t kStatusNoSuchFile = 0xC000000F;

    // ---- NT constants -------------------------------------------------------------------------
    constexpr uint32_t kGenericRead = 0x80000000;
    constexpr uint32_t kGenericWrite = 0x40000000;
    constexpr uint32_t kFileWriteData = 0x2;
    constexpr uint32_t kFileAppendData = 0x4;
    constexpr uint32_t kDelete = 0x10000;

    constexpr uint32_t kDispSupersede = 0, kDispOpen = 1, kDispCreate = 2, kDispOpenIf = 3, kDispOverwrite = 4,
                       kDispOverwriteIf = 5;

    constexpr uint32_t kOptDirectoryFile = 0x1;
    constexpr uint32_t kOptNonDirectoryFile = 0x40;

    // IO_STATUS_BLOCK.Information values for create/open
    constexpr uint32_t kInfoSuperseded = 0, kInfoOpened = 1, kInfoCreated = 2, kInfoOverwritten = 3;

    // FILE_INFORMATION_CLASS
    constexpr uint32_t kClassDirectory = 1, kClassBothDirectory = 3, kClassBasic = 4, kClassStandard = 5, kClassInternal = 6,
                       kClassEa = 7, kClassAccess = 8, kClassPosition = 14, kClassDisposition = 13, kClassEndOfFile = 20,
                       kClassNames = 12, kClassNetworkOpen = 34;

    constexpr uint64_t kUseCurrentPosition = 0xFFFFFFFFFFFFFFFEull;

    struct KFile : KObject
    {
        KFile() : KObject(KType::File) {}
        ~KFile()
        {
            if (find != INVALID_HANDLE_VALUE)
                FindClose(find);
        }

        std::wstring path;
        bool directory = false;
        bool writable = false;
        uint32_t options = 0;

        HANDLE find = INVALID_HANDLE_VALUE; // directory enumeration state
        bool findDone = false;
    };

    bool FsLogEnabled()
    {
        static const bool on = std::getenv("Q4_FSLOG") != nullptr;
        return on;
    }

    void SetIoStatus(uint32_t ptr, uint32_t status, uint32_t info)
    {
        if (!ptr)
            return;
        GuestStore32(ptr + 0, status);
        GuestStore32(ptr + 4, info);
    }

    std::string ReadAnsiString(uint32_t strPtr)
    {
        if (!strPtr)
            return std::string();

        const uint16_t length = GuestLoad16(strPtr);
        const uint32_t buffer = GuestLoad32(strPtr + 4);
        if (!buffer || length == 0)
            return std::string();

        return std::string(reinterpret_cast<const char*>(g_guestBase + buffer), length);
    }

    std::wstring Widen(const std::string& s)
    {
        if (s.empty())
            return std::wstring();
        const int n = MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_ACP, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::string Narrow(const wchar_t* w)
    {
        const int n = WideCharToMultiByte(CP_ACP, 0, w, -1, nullptr, 0, nullptr, nullptr);
        std::string s(n > 0 ? static_cast<size_t>(n - 1) : 0, '\0');
        if (n > 1)
            WideCharToMultiByte(CP_ACP, 0, w, -1, s.data(), n, nullptr, nullptr);
        return s;
    }

    uint32_t StatusFromLastError(DWORD err)
    {
        switch (err)
        {
        case ERROR_FILE_NOT_FOUND:     return nt::kObjectNameNotFound;
        case ERROR_PATH_NOT_FOUND:     return kStatusObjectPathNotFound;
        case ERROR_ACCESS_DENIED:      return kStatusAccessDenied;
        case ERROR_ALREADY_EXISTS:
        case ERROR_FILE_EXISTS:        return kStatusObjectNameCollision;
        case ERROR_SHARING_VIOLATION:  return 0xC0000043;
        case ERROR_HANDLE_EOF:         return nt::kEndOfFile;
        case ERROR_INVALID_NAME:       return 0xC0000033;
        default:                       return nt::kUnsuccessful;
        }
    }

    uint64_t FileTimeToU64(const FILETIME& ft)
    {
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    std::shared_ptr<KFile> GetFile(uint32_t handle)
    {
        auto obj = HandleGet(handle);
        if (!obj || obj->type != KType::File)
            return nullptr;
        return std::static_pointer_cast<KFile>(obj);
    }

    // OBJECT_ATTRIBUTES { root_directory, object_name (ANSI_STRING*), attributes }
    VfsPath ResolveObjectAttributes(uint32_t attrsPtr)
    {
        VfsPath bad;
        if (!attrsPtr)
            return bad;

        const uint32_t rootHandle = GuestLoad32(attrsPtr + 0);
        const uint32_t namePtr = GuestLoad32(attrsPtr + 4);
        const std::string name = ReadAnsiString(namePtr);

        // 0 and the 0xFFFFFFxx values are the kernel's "no root directory" markers, not handles.
        std::wstring relativeTo;
        if (rootHandle && rootHandle < 0xFFFFFFF0)
        {
            auto root = GetFile(rootHandle);
            if (!root)
            {
                bad.normalized = name;
                return bad;
            }
            relativeTo = root->path;
        }

        return VfsResolve(name, relativeTo);
    }

    void LogOpen(const char* api, const VfsPath& p, uint32_t status, uint32_t handle)
    {
        static unsigned failures = 0;
        const bool failed = status != nt::kSuccess;
        if (!FsLogEnabled() && !(failed && failures < 40))
            return;
        if (failed)
            failures++;

        std::printf("[fs] %s \"%s\" -> %s: status 0x%08X", api, p.normalized.c_str(),
                    p.ok ? Narrow(p.hostPath.c_str()).c_str() : "<unmapped>", status);
        if (handle)
            std::printf(" handle 0x%08X", handle);
        std::printf("\n");
        std::fflush(stdout);
    }

    void FillNetworkOpenInfo(uint32_t out, const WIN32_FILE_ATTRIBUTE_DATA& d)
    {
        const uint64_t size = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        GuestStore64(out + 0x00, FileTimeToU64(d.ftCreationTime));
        GuestStore64(out + 0x08, FileTimeToU64(d.ftLastAccessTime));
        GuestStore64(out + 0x10, FileTimeToU64(d.ftLastWriteTime));
        GuestStore64(out + 0x18, FileTimeToU64(d.ftLastWriteTime));
        GuestStore64(out + 0x20, (size + 0xFFF) & ~0xFFFull);
        GuestStore64(out + 0x28, size);
        GuestStore32(out + 0x30, d.dwFileAttributes);
        GuestStore32(out + 0x34, 0);
    }

    // Shared by NtCreateFile / NtOpenFile.
    uint32_t OpenOrCreate(uint32_t* handleOut, uint32_t desiredAccess, uint32_t attrsPtr, uint32_t ioStatusPtr,
                          uint32_t disposition, uint32_t options, const char* api)
    {
        const VfsPath p = ResolveObjectAttributes(attrsPtr);
        if (!p.ok)
        {
            LogOpen(api, p, nt::kObjectNameNotFound, 0);
            SetIoStatus(ioStatusPtr, nt::kObjectNameNotFound, 0);
            return nt::kObjectNameNotFound;
        }

        const bool wantsWrite = (desiredAccess & (kGenericWrite | kFileWriteData | kFileAppendData | kDelete)) != 0;
        const bool creates = disposition == kDispSupersede || disposition == kDispCreate || disposition == kDispOpenIf ||
                             disposition == kDispOverwrite || disposition == kDispOverwriteIf;

        // The game disc is read-only: creation/overwrite there is refused, plain opens are
        // downgraded to read access.
        if (!p.writable && !p.volumeRoot && creates && disposition != kDispOpenIf)
        {
            LogOpen(api, p, kStatusAccessDenied, 0);
            SetIoStatus(ioStatusPtr, kStatusAccessDenied, 0);
            return kStatusAccessDenied;
        }

        const DWORD attrs = GetFileAttributesW(p.hostPath.c_str());
        const bool exists = attrs != INVALID_FILE_ATTRIBUTES;
        const bool isDir = exists && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;

        auto file = std::make_shared<KFile>();
        file->path = p.hostPath;
        file->writable = p.writable;
        file->options = options;

        uint32_t info = kInfoOpened;
        uint32_t status = nt::kSuccess;

        if (p.volumeRoot || isDir || (options & kOptDirectoryFile))
        {
            // Directory.
            if (!exists)
            {
                if (p.writable && (disposition == kDispCreate || disposition == kDispOpenIf) && (options & kOptDirectoryFile))
                {
                    if (!CreateDirectoryW(p.hostPath.c_str(), nullptr))
                        status = StatusFromLastError(GetLastError());
                    else
                        info = kInfoCreated;
                }
                else
                {
                    status = nt::kObjectNameNotFound;
                }
            }
            else if (!isDir)
            {
                status = kStatusNotADirectory;
            }

            if (status == nt::kSuccess)
            {
                file->directory = true;
                file->host = CreateFileW(p.hostPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                         FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (file->host == INVALID_HANDLE_VALUE)
                {
                    file->host = nullptr;
                    status = StatusFromLastError(GetLastError());
                }
            }
        }
        else
        {
            // Regular file.
            if (options & kOptNonDirectoryFile && isDir)
                status = kStatusFileIsADirectory;

            DWORD create = OPEN_EXISTING;
            switch (disposition)
            {
            case kDispOpen:        create = OPEN_EXISTING;   break;
            case kDispCreate:      create = CREATE_NEW;      break;
            case kDispOpenIf:      create = OPEN_ALWAYS;     break;
            case kDispOverwrite:   create = TRUNCATE_EXISTING; break;
            case kDispOverwriteIf:
            case kDispSupersede:   create = CREATE_ALWAYS;   break;
            }

            if (status == nt::kSuccess)
            {
                DWORD access = GENERIC_READ;
                if (p.writable && wantsWrite)
                    access |= GENERIC_WRITE;

                // Writable volume: make sure the directory chain exists for creations.
                file->host = CreateFileW(p.hostPath.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, create, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
                if (file->host == INVALID_HANDLE_VALUE)
                {
                    file->host = nullptr;
                    const DWORD err = GetLastError();
                    status = err == ERROR_FILE_NOT_FOUND ? nt::kObjectNameNotFound : StatusFromLastError(err);
                    // Distinguish a missing file from a missing directory.
                    if (err == ERROR_PATH_NOT_FOUND)
                        status = kStatusObjectPathNotFound;
                }
                else if (!exists && (create == CREATE_NEW || create == OPEN_ALWAYS || create == CREATE_ALWAYS))
                {
                    info = kInfoCreated;
                }
                else if (exists && (create == CREATE_ALWAYS))
                {
                    info = disposition == kDispSupersede ? kInfoSuperseded : kInfoOverwritten;
                }
                else if (exists && create == TRUNCATE_EXISTING)
                {
                    info = kInfoOverwritten;
                }
            }
        }

        if (status != nt::kSuccess)
        {
            LogOpen(api, p, status, 0);
            SetIoStatus(ioStatusPtr, status, 0);
            return status;
        }

        const uint32_t handle = HandleCreate(file);
        if (handleOut)
            *handleOut = handle;

        LogOpen(api, p, nt::kSuccess, handle);
        SetIoStatus(ioStatusPtr, nt::kSuccess, info);
        return nt::kSuccess;
    }

    uint32_t Arg9(PPCContext& ctx) { return GuestLoad32(ctx.r1.u32 + 0x54); }
}

// NTSTATUS NtCreateFile(HANDLE* out, ACCESS_MASK access, OBJECT_ATTRIBUTES* attrs, IO_STATUS_BLOCK* iosb,
//                       LARGE_INTEGER* allocSize, ULONG fileAttrs, ULONG share, ULONG disposition, ULONG options)
PPC_FUNC(__imp__NtCreateFile)
{
    TraceImport("NtCreateFile", ctx);

    uint32_t handle = 0;
    const uint32_t status =
        OpenOrCreate(&handle, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, Arg9(ctx), "NtCreateFile");

    if (status == nt::kSuccess && ctx.r3.u32)
        GuestStore32(ctx.r3.u32, handle);
    ctx.r3.u64 = status;
}

// NTSTATUS NtOpenFile(HANDLE* out, ACCESS_MASK access, OBJECT_ATTRIBUTES* attrs, IO_STATUS_BLOCK* iosb,
//                     ULONG share, ULONG options)
PPC_FUNC(__imp__NtOpenFile)
{
    TraceImport("NtOpenFile", ctx);

    uint32_t handle = 0;
    const uint32_t status = OpenOrCreate(&handle, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, kDispOpen, ctx.r8.u32, "NtOpenFile");

    if (status == nt::kSuccess && ctx.r3.u32)
        GuestStore32(ctx.r3.u32, handle);
    ctx.r3.u64 = status;
}

// NTSTATUS NtReadFile(HANDLE h, HANDLE event, PVOID apc, PVOID apcCtx, IO_STATUS_BLOCK* iosb,
//                     PVOID buffer, ULONG length, LARGE_INTEGER* byteOffset)
PPC_FUNC(__imp__NtReadFile)
{
    auto file = GetFile(ctx.r3.u32);
    const uint32_t eventHandle = ctx.r4.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t offsetPtr = ctx.r10.u32;

    if (!file || file->directory || !file->host)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    OVERLAPPED ov{};
    bool explicitOffset = false;
    if (offsetPtr)
    {
        const uint64_t off = GuestLoad64(offsetPtr);
        if (off != kUseCurrentPosition && off != 0xFFFFFFFFFFFFFFFFull)
        {
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            explicitOffset = true;
        }
    }

    {
        static const bool log = std::getenv("Q4_FSLOG") != nullptr;
        static std::atomic<uint64_t> reads{ 0 }, bytes{ 0 }, lastTick{ 0 };
        if (log)
        {
            ++reads;
            bytes += length;
            static const uint32_t btHandle = [] {
                const char* e = std::getenv("Q4_BT_HANDLE");
                return e ? static_cast<uint32_t>(std::strtoul(e, nullptr, 16)) : 0u;
            }();
            if (btHandle && ctx.r3.u32 == btHandle)
                BacktraceDump(ctx, "read of watched handle");
            if (ctx.r5.u32 || ctx.r4.u32)
            {
                static std::atomic<unsigned> asyncSeen{ 0 };
                if (asyncSeen++ < 10)
                {
                    std::printf("[fs] async-style read: handle 0x%08X event 0x%08X apc 0x%08X apcctx 0x%08X len %u\n",
                                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, length);
                    std::fflush(stdout);
                }
            }
            const uint64_t now = GetTickCount64();
            uint64_t prev = lastTick.load();
            if (now - prev >= 1000 && lastTick.compare_exchange_strong(prev, now))
            {
                std::printf("[fs] reads=%llu bytes=%llu last: handle 0x%08X offset 0x%llX len %u\n",
                            static_cast<unsigned long long>(reads.load()), static_cast<unsigned long long>(bytes.load()),
                            ctx.r3.u32, static_cast<unsigned long long>(explicitOffset ? (uint64_t(ov.OffsetHigh) << 32 | ov.Offset) : 0),
                            length);
                std::fflush(stdout);
            }
        }
    }

    DWORD got = 0;
    BOOL ok;
    if (explicitOffset)
    {
        ok = ReadFile(file->host, g_guestBase + buffer, length, &got, &ov);
    }
    else
    {
        ok = ReadFile(file->host, g_guestBase + buffer, length, &got, nullptr);
    }

    uint32_t status = nt::kSuccess;
    if (!ok)
    {
        const DWORD err = GetLastError();
        status = err == ERROR_HANDLE_EOF ? nt::kEndOfFile : StatusFromLastError(err);
        got = 0;
    }
    else if (got == 0 && length != 0)
    {
        status = nt::kEndOfFile;
    }

    if (eventHandle)
    {
        if (auto ev = HandleGet(eventHandle); ev && ev->host)
            SetEvent(ev->host);
    }

    SetIoStatus(ioStatus, status, got);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(...same shape as NtReadFile...)
PPC_FUNC(__imp__NtWriteFile)
{
    auto file = GetFile(ctx.r3.u32);
    const uint32_t eventHandle = ctx.r4.u32;
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t buffer = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t offsetPtr = ctx.r10.u32;

    if (!file || file->directory || !file->host)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (!file->writable)
    {
        SetIoStatus(ioStatus, kStatusAccessDenied, 0);
        ctx.r3.u64 = kStatusAccessDenied;
        return;
    }

    OVERLAPPED ov{};
    bool explicitOffset = false;
    if (offsetPtr)
    {
        const uint64_t off = GuestLoad64(offsetPtr);
        if (off != kUseCurrentPosition && off != 0xFFFFFFFFFFFFFFFFull)
        {
            ov.Offset = static_cast<DWORD>(off);
            ov.OffsetHigh = static_cast<DWORD>(off >> 32);
            explicitOffset = true;
        }
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(file->host, g_guestBase + buffer, length, &written, explicitOffset ? &ov : nullptr);
    const uint32_t status = ok ? nt::kSuccess : StatusFromLastError(GetLastError());

    if (eventHandle)
    {
        if (auto ev = HandleGet(eventHandle); ev && ev->host)
            SetEvent(ev->host);
    }

    SetIoStatus(ioStatus, status, written);
    ctx.r3.u64 = status;
}

// NTSTATUS NtFlushBuffersFile(HANDLE h, IO_STATUS_BLOCK* iosb)
PPC_FUNC(__imp__NtFlushBuffersFile)
{
    auto file = GetFile(ctx.r3.u32);
    if (!file)
    {
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (file->host && file->writable)
        FlushFileBuffers(file->host);
    SetIoStatus(ctx.r4.u32, nt::kSuccess, 0);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtQueryInformationFile(HANDLE h, IO_STATUS_BLOCK* iosb, PVOID info, ULONG length, ULONG class)
PPC_FUNC(__imp__NtQueryInformationFile)
{
    TraceImport("NtQueryInformationFile", ctx);

    auto file = GetFile(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t out = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    const uint32_t cls = ctx.r7.u32;

    if (!file)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file->path.c_str(), GetFileExInfoStandard, &data))
    {
        const uint32_t st = StatusFromLastError(GetLastError());
        SetIoStatus(ioStatus, st, 0);
        ctx.r3.u64 = st;
        return;
    }

    const uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    auto need = [&](uint32_t n) { return length >= n && out != 0; };

    uint32_t status = nt::kSuccess;
    uint32_t info = 0;

    switch (cls)
    {
    case kClassBasic:
        if (!need(0x28)) { status = kStatusInfoLengthMismatch; break; }
        GuestStore64(out + 0x00, FileTimeToU64(data.ftCreationTime));
        GuestStore64(out + 0x08, FileTimeToU64(data.ftLastAccessTime));
        GuestStore64(out + 0x10, FileTimeToU64(data.ftLastWriteTime));
        GuestStore64(out + 0x18, FileTimeToU64(data.ftLastWriteTime));
        GuestStore32(out + 0x20, data.dwFileAttributes);
        GuestStore32(out + 0x24, 0);
        info = 0x28;
        break;

    case kClassStandard:
        if (!need(0x18)) { status = kStatusInfoLengthMismatch; break; }
        GuestStore64(out + 0x00, (size + 0xFFF) & ~0xFFFull);
        GuestStore64(out + 0x08, size);
        GuestStore32(out + 0x10, 1);
        g_guestBase[out + 0x14] = 0;
        g_guestBase[out + 0x15] = file->directory ? 1 : 0;
        g_guestBase[out + 0x16] = 0;
        g_guestBase[out + 0x17] = 0;
        info = 0x18;
        break;

    case kClassInternal:
        if (!need(8)) { status = kStatusInfoLengthMismatch; break; }
        GuestStore64(out, reinterpret_cast<uintptr_t>(file.get()));
        info = 8;
        break;

    case kClassEa:
        if (!need(4)) { status = kStatusInfoLengthMismatch; break; }
        GuestStore32(out, 0);
        info = 4;
        break;

    case kClassAccess:
        if (!need(4)) { status = kStatusInfoLengthMismatch; break; }
        GuestStore32(out, 0x001F01FF);
        info = 4;
        break;

    case kClassPosition:
    {
        if (!need(8)) { status = kStatusInfoLengthMismatch; break; }
        LARGE_INTEGER pos{}, zero{};
        SetFilePointerEx(file->host, zero, &pos, FILE_CURRENT);
        GuestStore64(out, static_cast<uint64_t>(pos.QuadPart));
        info = 8;
        break;
    }

    case kClassNetworkOpen:
        if (!need(0x38)) { status = kStatusInfoLengthMismatch; break; }
        FillNetworkOpenInfo(out, data);
        info = 0x38;
        break;

    default:
        status = nt::kNotImplemented;
        {
            static unsigned logged = 0;
            if (logged++ < 8)
                std::printf("[fs] NtQueryInformationFile: unhandled class %u\n", cls);
        }
        break;
    }

    SetIoStatus(ioStatus, status, info);
    ctx.r3.u64 = status;
}

// NTSTATUS NtSetInformationFile(HANDLE h, IO_STATUS_BLOCK* iosb, PVOID info, ULONG length, ULONG class)
PPC_FUNC(__imp__NtSetInformationFile)
{
    TraceImport("NtSetInformationFile", ctx);

    auto file = GetFile(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t in = ctx.r5.u32;
    const uint32_t cls = ctx.r7.u32;

    if (!file || !file->host)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    uint32_t status = nt::kSuccess;
    switch (cls)
    {
    case kClassPosition:
    {
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(GuestLoad64(in));
        SetFilePointerEx(file->host, pos, nullptr, FILE_BEGIN);
        break;
    }
    case kClassEndOfFile:
        if (file->writable)
        {
            LARGE_INTEGER eof;
            eof.QuadPart = static_cast<LONGLONG>(GuestLoad64(in));
            SetFilePointerEx(file->host, eof, nullptr, FILE_BEGIN);
            SetEndOfFile(file->host);
        }
        else
        {
            status = kStatusAccessDenied;
        }
        break;
    case kClassDisposition:
        break; // delete-on-close is ignored
    default:
        break;
    }

    SetIoStatus(ioStatus, status, 0);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES* attrs, FILE_NETWORK_OPEN_INFORMATION* out)
PPC_FUNC(__imp__NtQueryFullAttributesFile)
{
    TraceImport("NtQueryFullAttributesFile", ctx);

    const VfsPath p = ResolveObjectAttributes(ctx.r3.u32);
    if (!p.ok)
    {
        LogOpen("NtQueryFullAttributesFile", p, nt::kObjectNameNotFound, 0);
        ctx.r3.u64 = nt::kObjectNameNotFound;
        return;
    }

    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (p.volumeRoot)
    {
        data.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
    }
    else if (!GetFileAttributesExW(p.hostPath.c_str(), GetFileExInfoStandard, &data))
    {
        const DWORD err = GetLastError();
        const uint32_t st = err == ERROR_PATH_NOT_FOUND ? kStatusObjectPathNotFound : nt::kObjectNameNotFound;
        LogOpen("NtQueryFullAttributesFile", p, st, 0);
        ctx.r3.u64 = st;
        return;
    }

    if (ctx.r4.u32)
        FillNetworkOpenInfo(ctx.r4.u32, data);
    ctx.r3.u64 = nt::kSuccess;
}

// NTSTATUS NtQueryVolumeInformationFile(HANDLE h, IO_STATUS_BLOCK* iosb, PVOID info, ULONG length, ULONG class)
PPC_FUNC(__imp__NtQueryVolumeInformationFile)
{
    TraceImport("NtQueryVolumeInformationFile", ctx);

    auto file = GetFile(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r4.u32;
    const uint32_t out = ctx.r5.u32;
    const uint32_t length = ctx.r6.u32;
    const uint32_t cls = ctx.r7.u32;

    if (!file)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    uint32_t status = nt::kSuccess;
    uint32_t info = 0;

    switch (cls)
    {
    case 1: // FileFsVolumeInformation
        if (length < 0x18 || !out) { status = kStatusInfoLengthMismatch; break; }
        std::memset(g_guestBase + out, 0, 0x18);
        GuestStore32(out + 0x08, 0x1234ABCD); // serial number
        info = 0x18;
        break;

    case 3: // FileFsSizeInformation
        if (length < 0x18 || !out) { status = kStatusInfoLengthMismatch; break; }
        GuestStore64(out + 0x00, 0x400000);  // total allocation units
        GuestStore64(out + 0x08, 0x200000);  // available units
        GuestStore32(out + 0x10, 0x40);      // sectors per unit
        GuestStore32(out + 0x14, 0x200);     // bytes per sector
        info = 0x18;
        break;

    case 4: // FileFsDeviceInformation
        if (length < 8 || !out) { status = kStatusInfoLengthMismatch; break; }
        GuestStore32(out + 0, file->writable ? 7u : 3u); // disk : cd-rom file system
        GuestStore32(out + 4, file->writable ? 0u : 1u);  // read-only media
        info = 8;
        break;

    case 5: // FileFsAttributeInformation
        if (length < 0x0C || !out) { status = kStatusInfoLengthMismatch; break; }
        GuestStore32(out + 0, 0x6);   // case preserved + unicode
        GuestStore32(out + 4, 255);   // max component length
        GuestStore32(out + 8, 0);     // fs name length
        info = 0x0C;
        break;

    default:
        status = nt::kNotImplemented;
        break;
    }

    SetIoStatus(ioStatus, status, info);
    ctx.r3.u64 = status;
}

// NTSTATUS NtQueryDirectoryFile(HANDLE h, HANDLE event, PVOID apc, PVOID apcCtx, IO_STATUS_BLOCK* iosb,
//                               PVOID info, ULONG length, ANSI_STRING* fileName, BOOLEAN restart)
//
// Unlike the desktop NT call, the console kernel has no FileInformationClass parameter: the
// result is always an X_FILE_DIRECTORY_INFORMATION (ANSI file name at +0x40).
PPC_FUNC(__imp__NtQueryDirectoryFile)
{
    TraceImport("NtQueryDirectoryFile", ctx);

    auto file = GetFile(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r7.u32;
    const uint32_t out = ctx.r8.u32;
    const uint32_t length = ctx.r9.u32;
    const uint32_t cls = kClassDirectory;
    const uint32_t fileNamePtr = ctx.r10.u32;
    const bool restart = (Arg9(ctx) & 0xFF) != 0;

    if (!file || !file->directory)
    {
        SetIoStatus(ioStatus, nt::kInvalidHandle, 0);
        ctx.r3.u64 = nt::kInvalidHandle;
        return;
    }

    if (restart && file->find != INVALID_HANDLE_VALUE)
    {
        FindClose(file->find);
        file->find = INVALID_HANDLE_VALUE;
        file->findDone = false;
    }

    WIN32_FIND_DATAW fd{};
    bool have = false;

    if (file->findDone)
    {
        have = false;
    }
    else
    {
        if (file->find == INVALID_HANDLE_VALUE)
        {
            std::string pattern = ReadAnsiString(fileNamePtr);
            if (pattern.empty())
                pattern = "*";
            const std::wstring query = file->path + L"\\" + Widen(pattern);
            file->find = FindFirstFileW(query.c_str(), &fd);
            have = file->find != INVALID_HANDLE_VALUE;
        }
        else
        {
            have = FindNextFileW(file->find, &fd) != 0;
        }

        // The console file system does not report "." and "..".
        while (have && (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0))
            have = FindNextFileW(file->find, &fd) != 0;

        if (!have)
            file->findDone = true;
    }

    if (!have)
    {
        SetIoStatus(ioStatus, nt::kNoMoreFiles, 0);
        ctx.r3.u64 = nt::kNoMoreFiles;
        return;
    }

    const std::string name = Narrow(fd.cFileName);
    const uint32_t nameOffset = cls == kClassBothDirectory ? 0x5E : (cls == kClassNames ? 0x0C : 0x40);
    const uint32_t total = nameOffset + static_cast<uint32_t>(name.size());

    if (!out || length < total)
    {
        SetIoStatus(ioStatus, kStatusInfoLengthMismatch, 0);
        ctx.r3.u64 = kStatusInfoLengthMismatch;
        return;
    }

    std::memset(g_guestBase + out, 0, total);
    const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;

    GuestStore32(out + 0x00, 0); // NextEntryOffset: single entry per call
    GuestStore32(out + 0x04, 0); // FileIndex

    if (cls == kClassNames)
    {
        GuestStore32(out + 0x08, static_cast<uint32_t>(name.size()));
    }
    else
    {
        GuestStore64(out + 0x08, FileTimeToU64(fd.ftCreationTime));
        GuestStore64(out + 0x10, FileTimeToU64(fd.ftLastAccessTime));
        GuestStore64(out + 0x18, FileTimeToU64(fd.ftLastWriteTime));
        GuestStore64(out + 0x20, FileTimeToU64(fd.ftLastWriteTime));
        GuestStore64(out + 0x28, size);
        GuestStore64(out + 0x30, (size + 0xFFF) & ~0xFFFull);
        GuestStore32(out + 0x38, fd.dwFileAttributes);
        GuestStore32(out + 0x3C, static_cast<uint32_t>(name.size()));
    }

    std::memcpy(g_guestBase + out + nameOffset, name.data(), name.size());

    SetIoStatus(ioStatus, nt::kSuccess, total);
    ctx.r3.u64 = nt::kSuccess;
}
