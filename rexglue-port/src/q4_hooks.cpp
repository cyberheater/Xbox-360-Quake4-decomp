// q4 - project-level runtime tweaks.

#include "generated/default/q4_init.h"

#include <rex/cvar.h>


#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <cstdio>
#include <cstdlib>
#include <string>

REXCVAR_DEFINE_BOOL(q4_hide_cursor, true, "UI",
                    "Hide the mouse cursor while playing (it reappears briefly when the mouse moves)");

REXCVAR_DEFINE_BOOL(q4_show_fps, false, "UI",
                    "Show the FPS counter in the top-right corner (F9 toggles it)");

// Ask Windows for a 0.5 ms system timer resolution so guest sleeps, waits and timers
// wake promptly.
namespace {
struct Q4TimerRes {
  Q4TimerRes() {
    // Frame pacing: do not make the command processor wait for the vblank flag before a swap
    // (that wait stalls the game thread on GPU fences, giving a double-length frame every
    // ~21 frames); VdSwap paces to the vblank instead. Physical address of the game's flag.
    if (!std::getenv("Q4_SWAP_SKIP_ADDR")) _putenv("Q4_SWAP_SKIP_ADDR=1EDA6006");
    using Fn = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
    if (auto h = GetModuleHandleW(L"ntdll.dll")) {
      if (auto fn = reinterpret_cast<Fn>(GetProcAddress(h, "NtSetTimerResolution"))) {
        ULONG cur = 0;
        fn(5000, TRUE, &cur);
      }
    }
  }
} g_q4_timer_res;
}  // namespace

// Relaunch this executable (used by the settings menu's "Apply & Restart").
namespace q4 {
std::filesystem::path ConfigPath() {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "q4.toml";
  return std::filesystem::path(exe_path).parent_path() / "q4.toml";
}

[[noreturn]] void HardExit() {
  TerminateProcess(GetCurrentProcess(), 0);
  for (;;) {
  }
}

void LaunchSelfDetached() {
  wchar_t exe_path[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  std::wstring full(exe_path, exe_path + n);
  size_t slash = full.find_last_of(L"\\/");
  std::wstring workdir = (slash == std::wstring::npos) ? std::wstring() : full.substr(0, slash);
  ShellExecuteW(nullptr, L"open", exe_path, nullptr, workdir.empty() ? nullptr : workdir.c_str(),
                SW_SHOWNORMAL);
}
}  // namespace q4

// ---- Locating the game ------------------------------------------------------------------
namespace q4 {

bool IsValidGameLocation(const std::filesystem::path& p) {
  std::error_code ec;
  if (p.empty()) return false;
  if (std::filesystem::is_directory(p, ec)) {
    return std::filesystem::is_regular_file(p / "default.xex", ec);
  }
  if (std::filesystem::is_regular_file(p, ec)) {
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    return ext == ".iso" && std::filesystem::file_size(p, ec) > (100ull << 20);
  }
  return false;
}

std::optional<std::filesystem::path> PromptForGameLocation(const wchar_t* intro_message) {
  if (intro_message && *intro_message) {
    MessageBoxW(nullptr, intro_message, L"Quake 4", MB_OK | MB_ICONINFORMATION);
  }
  for (;;) {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Quake 4 ISO or default.xex\0*.iso;default.xex\0All files\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Locate your Quake 4 ISO (or default.xex from the extracted game)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;  // cancelled
    std::filesystem::path chosen = file;
    std::string name = chosen.filename().string();
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return std::tolower(c); });
    if (name == "default.xex") chosen = chosen.parent_path();
    if (IsValidGameLocation(chosen)) return chosen;
    if (MessageBoxW(nullptr,
                    L"That does not look like a Quake 4 (Xbox 360) ISO or extracted game folder.\n\n"
                    L"Choose the .iso file, or default.xex from the extracted game. Try again?",
                    L"Quake 4", MB_YESNO | MB_ICONWARNING) != IDYES) {
      return std::nullopt;
    }
  }
}

}  // namespace q4
