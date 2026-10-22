#pragma once
#include <string>

// Guest path -> host path mapping.
//
// The title addresses storage through Xbox device names and drive letters:
//   game:\ d:\ \Device\Cdrom0\          read-only game data (the extracted disc)
//   e:\ cache:\ hdd:\ \Device\Harddisk0\Partition1\ ...   writable save/cache area
// Anything else is looked up relative to the game root.

struct VfsPath
{
    bool ok = false;           // false: path could not be mapped (unknown device, escape attempt, ...)
    bool writable = false;     // lives on the writable (HDD) volume
    bool volumeRoot = false;   // the path names a volume itself, not an entry in it
    std::wstring hostPath;     // absolute Windows path
    std::string normalized;    // guest path after normalisation, for logging
};

void VfsInit(const std::wstring& gameRoot, const std::wstring& saveRoot);

// Maps a guest path. `relativeTo` is the host path of the root-directory handle (may be empty).
VfsPath VfsResolve(const std::string& guestPath, const std::wstring& relativeTo = std::wstring());
