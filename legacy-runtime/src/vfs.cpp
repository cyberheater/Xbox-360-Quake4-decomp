#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <vector>
#include <windows.h>

namespace
{
    std::wstring g_gameRoot;
    std::wstring g_saveRoot;

    std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool StartsWith(const std::string& s, const char* prefix)
    {
        const std::string p(prefix);
        return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
    }

    // Splits on '\\' and rejects "." / ".." escapes. Returns false if the path tries to leave the root.
    bool SplitComponents(const std::string& rest, std::vector<std::string>& out)
    {
        size_t i = 0;
        while (i <= rest.size())
        {
            size_t j = rest.find('\\', i);
            if (j == std::string::npos)
                j = rest.size();

            const std::string part = rest.substr(i, j - i);
            if (!part.empty() && part != ".")
            {
                if (part == "..")
                    return false;
                out.push_back(part);
            }
            i = j + 1;
        }
        return true;
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
}

void VfsInit(const std::wstring& gameRoot, const std::wstring& saveRoot)
{
    g_gameRoot = gameRoot;
    g_saveRoot = saveRoot;

    // Normalise to backslashes, no trailing separator.
    for (auto* root : { &g_gameRoot, &g_saveRoot })
    {
        std::replace(root->begin(), root->end(), L'/', L'\\');
        while (!root->empty() && root->back() == L'\\')
            root->pop_back();
    }

    std::error_code ec;
    std::filesystem::create_directories(g_saveRoot, ec);
}

VfsPath VfsResolve(const std::string& guestPath, const std::wstring& relativeTo)
{
    VfsPath out;

    std::string path = guestPath;
    std::replace(path.begin(), path.end(), '/', '\\');
    out.normalized = path;

    // NT namespace prefix.
    if (StartsWith(path, "\\??\\"))
        path.erase(0, 4);

    const std::string lower = Lower(path);

    struct Mount
    {
        const char* prefix;
        bool writable;
    };
    static const Mount kMounts[] = {
        { "\\device\\cdrom0", false },
        { "\\device\\cdrom1", false },
        { "\\device\\harddisk0\\partition1", true },
        { "\\device\\harddisk0\\cache0", true },
        { "\\device\\harddisk0\\cache1", true },
        { "\\device\\mu0", true },
        { "\\device\\mu1", true },
        { "game:", false },
        { "d:", false },
        { "dvd:", false },
        { "e:", true },
        { "cache:", true },
        { "hdd:", true },
        { "save:", true },
    };

    const Mount* mount = nullptr;
    size_t prefixLen = 0;
    for (const auto& m : kMounts)
    {
        if (StartsWith(lower, m.prefix))
        {
            mount = &m;
            prefixLen = std::string(m.prefix).size();
            break;
        }
    }

    std::wstring root;
    std::string rest;

    if (mount)
    {
        out.writable = mount->writable;
        root = mount->writable ? g_saveRoot : g_gameRoot;
        rest = path.substr(prefixLen);
    }
    else if (!relativeTo.empty())
    {
        // Relative to a directory handle: the caller passes its host path.
        root = relativeTo;
        rest = path;
        out.writable = root.compare(0, g_saveRoot.size(), g_saveRoot) == 0;
    }
    else if (!path.empty() && path[0] != '\\' && path.find(':') == std::string::npos)
    {
        // Bare relative path: the title's working directory is the game disc.
        root = g_gameRoot;
        rest = path;
    }
    else
    {
        return out; // unknown device
    }

    std::vector<std::string> parts;
    if (!SplitComponents(rest, parts))
        return out;

    out.volumeRoot = parts.empty() && mount != nullptr;
    out.hostPath = root;
    for (const auto& part : parts)
    {
        out.hostPath += L'\\';
        out.hostPath += Widen(part);
    }

    out.ok = true;
    return out;
}
