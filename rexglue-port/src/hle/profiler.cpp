// q4hle - sampling profiler for the recompiled game code (enable with Q4_PROF=1).
//
// A background thread samples the process's threads ~2000 times a second: it suspends a thread,
// reads its registers, walks its stack (x64 unwind data) and maps every return address that lies
// in the recompiled code to the guest function it belongs to (PPCFuncMappings). Samples are tied
// to frames (FrameEnd() is called from the Swap hook), and each completed frame's samples go to a
// "slow" histogram (frame longer than Q4_PROF_SLOW_MS, default 1.25 x the tic interval) or a
// "normal" one. Every 10 s prof.txt in Q4_TRACE_DIR is rewritten with, per busy thread:
//   - share of samples running guest code / host code / waiting,
//   - top guest functions by exclusive samples (where the CPU actually is),
//   - top guest functions by inclusive samples (what the time is spent under),
//   - top host locations (module + RVA; resolve exe RVAs with q4hle.map).
// Reports compare slow frames with normal frames so the cause of a dip stands out.

#include "hle/profiler.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hle/log.h"
#include "hle/tickrate.h"

namespace q4hle::prof {
namespace {

constexpr int kMaxDepth = 48;

struct HostFn {
  uintptr_t host;
  uint32_t guest;
};
std::vector<HostFn> g_fns;  // sorted by host address
std::unordered_map<uintptr_t, uint32_t> g_exact;  // host function start -> guest address (read-only)
uintptr_t g_code_lo = 0, g_code_hi = 0;

uint32_t GuestOf(uintptr_t pc) {
  if (pc < g_code_lo || pc >= g_code_hi) return 0;
  auto it = std::upper_bound(g_fns.begin(), g_fns.end(), pc, [](uintptr_t v, const HostFn& f) { return v < f.host; });
  if (it == g_fns.begin()) return 0;
  return (it - 1)->guest;
}

struct Sample {
  uint64_t t;       // QPC
  uint32_t tid;
  uint8_t kind;     // 0 guest, 1 host (not waiting), 2 waiting
  uint8_t depth;    // guest frames
  uint32_t self;    // guest function the CPU is in (or under, for host code)
  uintptr_t host_pc;  // top-of-stack pc when it is not guest code
  uintptr_t host_caller;  // first frame (below the top) in rexruntime.dll or the exe's own (non-guest) code
  uint32_t stack[kMaxDepth];
};

struct Hist {
  uint64_t total = 0, guest = 0, host = 0, wait = 0;
  std::unordered_map<uint32_t, uint64_t> self, incl;
  std::unordered_map<uint64_t, uint64_t> hostpc;  // (module index << 32) | rva
  std::unordered_map<uint64_t, uint64_t> hostcaller;  // caller in rexruntime / exe of host code
  std::unordered_map<uint32_t, uint64_t> waitg;  // guest function under which the thread waits
  std::unordered_map<uint32_t, uint64_t> waitincl;  // every guest function on the stack while waiting
  std::unordered_map<uint64_t, uint64_t> waith;  // runtime / exe frame that waits
};
struct ThreadStats {
  Hist slow, normal;
};

std::mutex g_mu;
std::unordered_map<uint32_t, ThreadStats> g_threads;
uint64_t g_frames_slow = 0, g_frames_normal = 0;
double g_ms_slow = 0, g_ms_normal = 0;

// frame boundaries from the game thread (QPC), single producer
constexpr size_t kFrameRing = 4096;
uint64_t g_frame_t[kFrameRing];
std::atomic<uint64_t> g_frame_n{0};
uint32_t g_game_tid = 0;

std::vector<std::pair<uintptr_t, std::string>> g_modules;  // base, name
int g_exe_module = -1, g_rt_module = -1;
std::vector<uintptr_t> g_module_end;

void LoadModules() {
  g_modules.clear();
  g_module_end.clear();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return;
  MODULEENTRY32W me{};
  me.dwSize = sizeof(me);
  for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me)) {
    char name[260];
    WideCharToMultiByte(CP_UTF8, 0, me.szModule, -1, name, sizeof(name), nullptr, nullptr);
    g_modules.emplace_back(reinterpret_cast<uintptr_t>(me.modBaseAddr), name);
    g_module_end.push_back(reinterpret_cast<uintptr_t>(me.modBaseAddr) + me.modBaseSize);
  }
  CloseHandle(snap);
  g_exe_module = g_rt_module = -1;
  for (size_t i = 0; i < g_modules.size(); ++i) {
    if (_stricmp(g_modules[i].second.c_str(), "q4hle.exe") == 0) g_exe_module = int(i);
    if (_stricmp(g_modules[i].second.c_str(), "rexruntime.dll") == 0) g_rt_module = int(i);
  }
}

int ModuleOf(uintptr_t pc) {
  for (size_t i = 0; i < g_modules.size(); ++i)
    if (pc >= g_modules[i].first && pc < g_module_end[i]) return int(i);
  return -1;
}

bool IsWaitModule(int m) {
  if (m < 0) return false;
  const std::string& n = g_modules[m].second;
  return _stricmp(n.c_str(), "ntdll.dll") == 0 || _stricmp(n.c_str(), "win32u.dll") == 0 ||
         _stricmp(n.c_str(), "kernelbase.dll") == 0;
}

// Takes one sample of a suspended thread. No allocation (the thread may hold the heap lock).
bool SampleThread(HANDLE th, uint32_t tid, Sample& s) {
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
  if (!GetThreadContext(th, &ctx)) return false;
  s.tid = tid;
  s.depth = 0;
  s.self = 0;
  s.host_pc = 0;
  s.host_caller = 0;
  uint32_t first_guest = 0;
  bool top = true;
  bool top_is_guest = false;
  for (int i = 0; i < 64 && ctx.Rip; ++i) {
    const uintptr_t pc = ctx.Rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &image_base, nullptr);
    uint32_t g = 0;
    if (fe) {
      // the unwind table gives the exact function start: guest code only if it is a mapped function
      auto it = g_exact.find(uintptr_t(image_base + fe->BeginAddress));
      if (it != g_exact.end()) g = it->second;
      else if (pc >= g_code_lo && pc < g_code_hi && !(g_exact.empty())) g = 0;
    } else {
      g = GuestOf(pc);  // leaf function without unwind data
    }
    if (top) {
      top_is_guest = g != 0;
      if (!g) s.host_pc = pc;
      top = false;
    }
    if (!g && !top && !s.host_caller && !first_guest) {
      const int m = ModuleOf(pc);
      if (m >= 0 && (m == g_exe_module || m == g_rt_module)) s.host_caller = pc;
    }
    if (g) {
      if (!first_guest) first_guest = g;
      if (s.depth < kMaxDepth && (s.depth == 0 || s.stack[s.depth - 1] != g)) s.stack[s.depth++] = g;
    }
    if (!fe) {
      // leaf function: return address is at [rsp]
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, pc, fe, &ctx, &handler_data, &establisher, nullptr);
  }
  s.self = first_guest;
  if (top_is_guest) s.kind = 0;
  else s.kind = IsWaitModule(ModuleOf(s.host_pc)) ? 2 : 1;
  return true;
}

void Add(Hist& h, const Sample& s) {
  ++h.total;
  auto key_of = [](uintptr_t pc) -> uint64_t {
    const int m = ModuleOf(pc);
    return m < 0 ? 0xFFFFFFFF00000000ull : (uint64_t(m) << 32) | uint32_t(pc - g_modules[m].first);
  };
  if (s.kind == 2) {
    ++h.wait;
    ++h.waitg[s.self];
    for (int i = 0; i < s.depth; ++i) {
      bool dup = false;
      for (int j = 0; j < i; ++j)
        if (s.stack[j] == s.stack[i]) dup = true;
      if (!dup) ++h.waitincl[s.stack[i]];
    }
    if (s.host_caller) ++h.waith[key_of(s.host_caller)];
    return;
  }
  if (s.kind == 1 && s.host_caller) ++h.hostcaller[key_of(s.host_caller)];
  if (s.kind == 0) ++h.guest;
  else {
    ++h.host;
    const int m = ModuleOf(s.host_pc);
    const uint64_t key = m < 0 ? 0xFFFFFFFF00000000ull : (uint64_t(m) << 32) | uint32_t(s.host_pc - g_modules[m].first);
    ++h.hostpc[key];
  }
  if (s.self) ++h.self[s.self];
  // inclusive: each distinct function once per sample
  for (int i = 0; i < s.depth; ++i) {
    bool dup = false;
    for (int j = 0; j < i; ++j)
      if (s.stack[j] == s.stack[i]) dup = true;
    if (!dup) ++h.incl[s.stack[i]];
  }
}

template <typename K>
std::vector<std::pair<K, uint64_t>> Top(const std::unordered_map<K, uint64_t>& m, size_t n) {
  std::vector<std::pair<K, uint64_t>> v(m.begin(), m.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
  if (v.size() > n) v.resize(n);
  return v;
}

void WriteReport() {
  std::string dir = ".";
  if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
  FILE* f = std::fopen((dir + "/prof.txt").c_str(), "w");
  if (!f) return;
  std::lock_guard<std::mutex> lk(g_mu);
  std::fprintf(f, "frames: slow %llu (avg %.2f ms), normal %llu (avg %.2f ms); game thread %u\n",
               (unsigned long long)g_frames_slow, g_frames_slow ? g_ms_slow / g_frames_slow : 0.0,
               (unsigned long long)g_frames_normal, g_frames_normal ? g_ms_normal / g_frames_normal : 0.0,
               g_game_tid);
  std::vector<std::pair<uint32_t, uint64_t>> order;
  for (auto& [tid, ts] : g_threads) order.push_back({tid, ts.slow.guest + ts.slow.host + ts.normal.guest + ts.normal.host});
  std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.second > b.second; });
  int shown = 0;
  for (auto& [tid, busy] : order) {
    if (busy < 50 || shown++ >= 8) break;
    ThreadStats& ts = g_threads[tid];
    for (int w = 0; w < 2; ++w) {
      Hist& h = w == 0 ? ts.slow : ts.normal;
      if (!h.total) continue;
      std::fprintf(f, "\n=== thread %u%s, %s frames: %llu samples, guest %.1f%% host %.1f%% waiting %.1f%%\n", tid,
                   tid == g_game_tid ? " (game)" : "", w == 0 ? "SLOW" : "normal", (unsigned long long)h.total,
                   100.0 * h.guest / h.total, 100.0 * h.host / h.total, 100.0 * h.wait / h.total);
      const double busy_n = double(h.guest + h.host);
      std::fprintf(f, "-- exclusive (guest function the CPU is in, incl. host helpers it called)\n");
      for (auto& [g, n] : Top(h.self, 40)) std::fprintf(f, "  %08X %6.2f%%  %llu\n", g, 100.0 * n / busy_n, (unsigned long long)n);
      std::fprintf(f, "-- inclusive\n");
      for (auto& [g, n] : Top(h.incl, 60)) std::fprintf(f, "  %08X %6.2f%%  %llu\n", g, 100.0 * n / busy_n, (unsigned long long)n);
      auto print_host = [&](const std::unordered_map<uint64_t, uint64_t>& m, const char* title, double denom) {
        std::fprintf(f, "%s\n", title);
        for (auto& [k, n] : Top(m, 20)) {
          const uint32_t mi = uint32_t(k >> 32);
          std::fprintf(f, "  %s+%08X %6.2f%%  %llu\n", mi < g_modules.size() ? g_modules[mi].second.c_str() : "?",
                       uint32_t(k), 100.0 * n / denom, (unsigned long long)n);
        }
      };
      if (h.wait) {
        std::fprintf(f, "-- waiting, by guest function (%% of all samples)\n");
        for (auto& [g, n] : Top(h.waitg, 15)) std::fprintf(f, "  %08X %6.2f%%  %llu\n", g, 100.0 * n / h.total, (unsigned long long)n);
        print_host(h.waith, "-- waiting, by runtime/exe frame (% of all samples)", double(h.total));
        std::fprintf(f, "-- waiting, inclusive (%% of all samples)\n");
        for (auto& [g, n] : Top(h.waitincl, 30)) std::fprintf(f, "  %08X %6.2f%%  %llu\n", g, 100.0 * n / h.total, (unsigned long long)n);
      }
      print_host(h.hostcaller, "-- host code, by calling runtime/exe frame", busy_n);
      std::fprintf(f, "-- host code at top of stack (module+rva)\n");
      for (auto& [k, n] : Top(h.hostpc, 25)) {
        const uint32_t m = uint32_t(k >> 32);
        std::fprintf(f, "  %s+%08X %6.2f%%  %llu\n", m < g_modules.size() ? g_modules[m].second.c_str() : "?",
                     uint32_t(k), 100.0 * n / busy_n, (unsigned long long)n);
      }
    }
  }
  std::fclose(f);
}

void ProfThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
  if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const uint32_t self_tid = GetCurrentThreadId();
  double slow_ms = 1250.0 / double(q4hle::tick::Hz());
  if (const char* e = std::getenv("Q4_PROF_SLOW_MS")) slow_ms = std::atof(e);
  const uint64_t slow_ticks = uint64_t(slow_ms * double(freq.QuadPart) / 1000.0);

  std::vector<std::pair<uint32_t, HANDLE>> threads;
  std::unordered_map<uint32_t, uint32_t> idle_streak;  // consecutive waiting samples per thread
  idle_streak.reserve(256);
  uint64_t round = 0;
  std::vector<Sample> pending;  // samples of frames not yet complete
  pending.reserve(1 << 16);
  uint64_t frames_seen = 0;
  uint64_t last_report = GetTickCount64(), last_enum = 0;
  Sample s;
  for (;;) {
    const uint64_t now_ms = GetTickCount64();
    if (now_ms - last_enum > 2000) {
      last_enum = now_ms;
      LoadModules();
      for (auto& t : threads) CloseHandle(t.second);
      threads.clear();
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      THREADENTRY32 te{};
      te.dwSize = sizeof(te);
      for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self_tid) continue;
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (h) threads.push_back({te.th32ThreadID, h});
      }
      CloseHandle(snap);
    }
    for (auto& [tid, h] : threads) {
      uint32_t& idle = idle_streak[tid];  // (allocates before suspending, never while a thread is stopped)
      if (idle > 40 && (round & 15) != 0) continue;  // mostly-idle threads: sample 1 round in 16
      if (SuspendThread(h) == DWORD(-1)) continue;
      LARGE_INTEGER t;
      QueryPerformanceCounter(&t);
      s.t = uint64_t(t.QuadPart);
      const bool ok = SampleThread(h, tid, s);
      ResumeThread(h);
      if (ok) {
        idle = s.kind == 2 ? idle + 1 : 0;
        pending.push_back(s);
      }
    }
    // file samples of completed frames
    const uint64_t fn = g_frame_n.load(std::memory_order_acquire);
    // frame k spans [g_frame_t[k-1], g_frame_t[k]); frames_seen = next frame index to file
    if (frames_seen == 0) frames_seen = 1;
    if (fn > frames_seen) {
      if (fn - frames_seen > kFrameRing - 16) frames_seen = fn - (kFrameRing - 16);
      std::lock_guard<std::mutex> lk(g_mu);
      size_t consumed = 0;
      for (uint64_t k = frames_seen; k < fn; ++k) {
        const uint64_t a = g_frame_t[(k - 1) % kFrameRing], b = g_frame_t[k % kFrameRing];
        const bool slow = b - a > slow_ticks;
        const double ms = double(b - a) * 1000.0 / double(freq.QuadPart);
        static const uint64_t first_frame_t = a;
        const bool startup = double(a - first_frame_t) < 10.0 * double(freq.QuadPart);  // boot / first load
        const bool loading = ms > 250.0 || startup;  // loads and pauses: not a frame-rate dip
        if (!loading) {
          if (slow) ++g_frames_slow, g_ms_slow += ms;
          else ++g_frames_normal, g_ms_normal += ms;
        }
        // per-second timeline: frames, worst frame, frames over the slow threshold
        {
          static uint64_t sec_start = b, sec_frames = 0, sec_slow = 0;
          static double sec_max = 0;
          static FILE* tl = nullptr;
          static double t_origin = double(b);
          if (!tl) {
            std::string dir = ".";
            if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
            tl = std::fopen((dir + "/prof_timeline.csv").c_str(), "w");
            if (tl) std::fprintf(tl, "time_s,fps,worst_frame_ms,slow_frames\n");
          }
          ++sec_frames;
          if (slow && !loading) ++sec_slow;
          if (!loading) sec_max = std::max(sec_max, ms);
          if (b - sec_start >= uint64_t(freq.QuadPart)) {
            const double secs = double(b - sec_start) / double(freq.QuadPart);
            if (tl) {
              std::fprintf(tl, "%.1f,%.1f,%.2f,%llu\n", (double(b) - t_origin) / double(freq.QuadPart),
                           double(sec_frames) / secs, sec_max, (unsigned long long)sec_slow);
              std::fflush(tl);
            }
            sec_start = b;
            sec_frames = sec_slow = 0;
            sec_max = 0;
          }
        }
        while (consumed < pending.size() && pending[consumed].t < b) {
          const Sample& p = pending[consumed++];
          if (p.t < a || loading) continue;
          ThreadStats& ts = g_threads[p.tid];
          Add(slow ? ts.slow : ts.normal, p);
        }
      }
      pending.erase(pending.begin(), pending.begin() + consumed);
      frames_seen = fn;
    }
    if (pending.size() > (1 << 20)) pending.clear();  // no frames (loading): drop
    if (now_ms - last_report > 10000) {
      last_report = now_ms;
      WriteReport();
    }
    // ~2 kHz
    ++round;
    LARGE_INTEGER due;
    due.QuadPart = -5000;  // 0.5 ms (100 ns units, relative)
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 5);
    else Sleep(1);
  }
}

std::once_flag g_once;
bool g_enabled = false;

void Init() {
  const char* e = std::getenv("Q4_PROF");
  g_enabled = e && *e == '1';
  if (!g_enabled) return;
  for (const PPCFuncMapping* m = PPCFuncMappings; m->host; ++m)
    g_fns.push_back({reinterpret_cast<uintptr_t>(m->host), uint32_t(m->guest)});
  std::sort(g_fns.begin(), g_fns.end(), [](const HostFn& a, const HostFn& b) { return a.host < b.host; });
  g_exact.reserve(g_fns.size() * 2);
  for (const HostFn& f : g_fns) g_exact.emplace(f.host, f.guest);
  // the recompiled code: from the first to the last function (+ the last one's body)
  g_code_lo = g_fns.front().host;
  g_code_hi = g_fns.back().host + 0x10000;
  g_game_tid = GetCurrentThreadId();
  Log("profiler: %zu guest functions, host code %p-%p", g_fns.size(), (void*)g_code_lo, (void*)g_code_hi);
  std::thread(ProfThread).detach();
}

}  // namespace

void FrameEnd() {
  std::call_once(g_once, Init);
  if (!g_enabled) return;
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  const uint64_t n = g_frame_n.load(std::memory_order_relaxed);
  g_frame_t[n % kFrameRing] = uint64_t(t.QuadPart);
  g_frame_n.store(n + 1, std::memory_order_release);
}

}  // namespace q4hle::prof
