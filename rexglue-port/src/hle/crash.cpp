// q4hle - logs unhandled native exceptions (address relative to q4hle.exe) to q4hle.log.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdio>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <unordered_map>

#include "hle/log.h"

namespace q4hle::rb { const char* ThreadRole(); }  // rb_thread.cpp

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;

LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* ep) {
  const auto* er = ep->ExceptionRecord;
  // Ignore the debugger/C++ exception codes; log access violations and friends.
  if (er->ExceptionCode == 0xE06D7363 || er->ExceptionCode == 0x406D1388 || er->ExceptionCode < 0x80000000u)
    return EXCEPTION_CONTINUE_SEARCH;
  const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  const uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
  HMODULE mod = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(at), &mod);
  wchar_t name[MAX_PATH] = L"?";
  if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
  char narrow[MAX_PATH];
  WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, MAX_PATH, nullptr, nullptr);
  q4hle::Log("CRASH: exception %08lX at %p (%s + 0x%llx, exe base %p) info %llx %llx", er->ExceptionCode,
             er->ExceptionAddress, narrow,
             static_cast<unsigned long long>(at - reinterpret_cast<uintptr_t>(mod)), reinterpret_cast<void*>(exe),
             static_cast<unsigned long long>(er->NumberParameters > 0 ? er->ExceptionInformation[0] : 0),
             static_cast<unsigned long long>(er->NumberParameters > 1 ? er->ExceptionInformation[1] : 0));
  // which thread, and the guest call chain (host frames mapped back to recompiled functions)
  char chain[1024];
  int n = 0;
  {
    std::unordered_map<uintptr_t, uint32_t> fns;
    fns.reserve(32768);
    for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m) fns.emplace(reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest));
    CONTEXT c = *ep->ContextRecord;
    for (int depth = 0; depth < 24 && n < 960; ++depth) {
      DWORD64 image_base = 0;
      PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(c.Rip, &image_base, nullptr);
      if (!fe) {
        // leaf function without unwind data (e.g. a breakpoint stub): return address is at rsp
        n += std::snprintf(chain + n, sizeof(chain) - n, " leaf@%p", reinterpret_cast<void*>(c.Rip));
        c.Rip = *reinterpret_cast<DWORD64*>(c.Rsp);
        c.Rsp += 8;
        if (!c.Rip) break;
        continue;
      }
      auto it = fns.find(uintptr_t(image_base + fe->BeginAddress));
      if (it != fns.end()) n += std::snprintf(chain + n, sizeof(chain) - n, " %08X", it->second);
      else n += std::snprintf(chain + n, sizeof(chain) - n, " h+%llx", (unsigned long long)(c.Rip - image_base));
      void* hd = nullptr;
      DWORD64 est = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, c.Rip, fe, &c, &hd, &est, nullptr);
      if (!c.Rip) break;
    }
  }
  chain[n] = 0;
  q4hle::Log("CRASH: thread %lu (%s), stack:%s", GetCurrentThreadId(), q4hle::rb::ThreadRole(), chain);
  return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

namespace q4hle {
// Installs the unhandled-exception logger (call once the runtime has set up its own handlers;
// handled exceptions, such as the GPU emulation's memory watches, are not logged).
void InstallCrashLogger() {
  static bool installed = false;
  if (installed) return;
  installed = true;
  g_previous = SetUnhandledExceptionFilter(OnUnhandled);
}
}  // namespace q4hle

namespace {

}  // namespace
