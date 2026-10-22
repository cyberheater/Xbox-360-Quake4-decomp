// q4hle - D3D call tracing.
//
// Environment:
//   Q4_TRACE=1            enable
//   Q4_TRACE_DIR=<dir>    where to write (default: current directory)
//   Q4_TRACE_SWAP=<idx>   index of the swap function in kFuncAddr (frame boundary)
//   Q4_TRACE_FRAME=<n>    first frame whose full call sequence is logged (default 1500)
//   Q4_TRACE_FRAMES=<n>   number of frames to log in sequence (default 2)
//
// Output:
//   trace_stats.txt  rewritten every 120 frames: per-function call counts (total, top-level,
//                    calls per frame over the last window), most common callers, sample args
//   trace_seq.txt    ordered top-level D3D calls for the logged frames, with caller and r3-r8

#include "hle/trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <chrono>
#include <thread>

namespace q4hle::trace {
namespace {

constexpr unsigned kMax = 1024;

struct Stat {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> top{0};
  uint64_t window_base = 0;
  double per_frame = 0;
  std::array<uint32_t, 6> sample{};
};

struct State {
  bool enabled = false;
  unsigned swap_idx = ~0u;
  uint64_t seq_first = 1500, seq_count = 2;
  std::string dir = ".";
  std::array<Stat, kMax> stats;
  std::atomic<uint64_t> frame{0};
  std::mutex mu;  // guards callers, seq, file writes
  std::unordered_map<uint64_t, uint64_t> callers;  // (idx<<32 | lr) -> count
  std::vector<std::string> seq;
  bool seq_written = false;
};

State& S() {
  static State s;
  return s;
}

struct Init {
  Init() {
    State& s = S();
    const char* e = std::getenv("Q4_TRACE");
    s.enabled = e && *e && *e != '0';
    if (const char* d = std::getenv("Q4_TRACE_DIR")) s.dir = d;
    if (const char* v = std::getenv("Q4_TRACE_SWAP")) s.swap_idx = static_cast<unsigned>(std::atoi(v));
    if (const char* v = std::getenv("Q4_TRACE_FRAME")) s.seq_first = std::strtoull(v, nullptr, 10);
    if (const char* v = std::getenv("Q4_TRACE_FRAMES")) s.seq_count = std::strtoull(v, nullptr, 10);
  }
} g_init;

thread_local int t_depth = 0;

// ---- per-thread D3D call stacks for the hang watchdog ----
struct ThreadInfo {
  uint32_t tid = 0;
  std::atomic<int> depth{0};
  uint16_t stack[64] = {};
  std::atomic<uint32_t> last_lr{0};
  std::atomic<uint64_t> calls{0};
};
std::mutex g_threads_mu;
std::vector<ThreadInfo*> g_threads;
std::atomic<uint32_t> g_next_tid{1};
thread_local ThreadInfo* t_info = nullptr;
std::atomic<uint64_t> g_frames{0};

ThreadInfo* Info() {
  if (!t_info) {
    t_info = new ThreadInfo();
    t_info->tid = g_next_tid.fetch_add(1);
    std::lock_guard<std::mutex> lock(g_threads_mu);
    g_threads.push_back(t_info);
  }
  return t_info;
}

void WatchdogMain(std::string path) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f) continue;
    std::fprintf(f, "frames %llu\n", static_cast<unsigned long long>(g_frames.load()));
    std::lock_guard<std::mutex> lock(g_threads_mu);
    for (ThreadInfo* t : g_threads) {
      const int d = t->depth.load();
      std::fprintf(f, "thread %u calls %llu last_top_lr %08X depth %d :", t->tid,
                   static_cast<unsigned long long>(t->calls.load()), t->last_lr.load(), d);
      for (int i = 0; i < d && i < 64; ++i) std::fprintf(f, " %s", kFuncName[t->stack[i]]);
      std::fprintf(f, "\n");
    }
    std::fclose(f);
  }
}

struct WatchInit {
  WatchInit() {
    const char* e = std::getenv("Q4_WATCH");
    if (!e || !*e || *e == '0') return;
    std::string dir = ".";
    if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
    std::thread(WatchdogMain, dir + "/watch.txt").detach();
  }
} g_watch_init;

void WriteStats(uint64_t frame) {
  State& s = S();
  std::string path = s.dir + "/trace_stats.txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  std::fprintf(f, "frame %llu\n", static_cast<unsigned long long>(frame));
  std::fprintf(f, "idx addr name calls top per_frame_last_window sample_r3..r8 | top callers\n");
  // collect callers per idx
  std::vector<std::vector<std::pair<uint64_t, uint32_t>>> by(kNumFuncs);
  for (auto& [k, n] : s.callers) {
    unsigned idx = static_cast<unsigned>(k >> 32);
    if (idx < kNumFuncs) by[idx].push_back({n, static_cast<uint32_t>(k)});
  }
  for (unsigned i = 0; i < kNumFuncs && i < kMax; ++i) {
    Stat& st = s.stats[i];
    uint64_t c = st.calls.load(std::memory_order_relaxed);
    if (!c) continue;
    std::fprintf(f, "%u %08X %s %llu %llu %.2f", i, kFuncAddr[i], kFuncName[i],
                 static_cast<unsigned long long>(c),
                 static_cast<unsigned long long>(st.top.load(std::memory_order_relaxed)), st.per_frame);
    for (uint32_t v : st.sample) std::fprintf(f, " %08X", v);
    auto& cl = by[i];
    std::sort(cl.begin(), cl.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::fprintf(f, " |");
    for (size_t j = 0; j < cl.size() && j < 6; ++j)
      std::fprintf(f, " %08X:%llu", cl[j].second, static_cast<unsigned long long>(cl[j].first));
    std::fprintf(f, "\n");
  }
  std::fclose(f);
}

void WriteSeq() {
  State& s = S();
  std::string path = s.dir + "/trace_seq.txt";
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) return;
  for (auto& l : s.seq) std::fputs(l.c_str(), f);
  std::fclose(f);
}

}  // namespace

void (*g_enter_check)(unsigned idx, uint32_t lr) = nullptr;
void (*g_leave_check)(unsigned idx) = nullptr;

void Enter(unsigned idx, const PPCContext& ctx) {
  {
    ThreadInfo* ti = Info();
    const int d = ti->depth.load(std::memory_order_relaxed);
    if (d == 0 && g_enter_check) g_enter_check(idx, static_cast<uint32_t>(ctx.lr));
    if (d < 64) ti->stack[d] = static_cast<uint16_t>(idx);
    if (d == 0) ti->last_lr.store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
    ti->depth.store(d + 1, std::memory_order_relaxed);
    ti->calls.fetch_add(1, std::memory_order_relaxed);
  }
  State& s = S();
  if (!s.enabled || idx >= kMax) {
    ++t_depth;
    return;
  }
  Stat& st = s.stats[idx];
  st.calls.fetch_add(1, std::memory_order_relaxed);
  const bool top = (t_depth == 0);
  if (top) {
    st.top.fetch_add(1, std::memory_order_relaxed);
    st.sample = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
    const uint64_t frame = s.frame.load(std::memory_order_relaxed);
    const uint32_t lr = static_cast<uint32_t>(ctx.lr);
    std::lock_guard<std::mutex> lock(s.mu);
    if ((st.top.load(std::memory_order_relaxed) & 0x3F) == 1 || s.callers.size() < 200000)
      ++s.callers[(static_cast<uint64_t>(idx) << 32) | lr];
    if (frame >= s.seq_first && frame < s.seq_first + s.seq_count) {
      char buf[256];
      std::snprintf(buf, sizeof(buf), "%llu %s lr=%08X r3=%08X r4=%08X r5=%08X r6=%08X r7=%08X r8=%08X\n",
                    static_cast<unsigned long long>(frame), kFuncName[idx], lr, ctx.r3.u32, ctx.r4.u32,
                    ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
      s.seq.emplace_back(buf);
    }
  }
  ++t_depth;
}

void Leave(unsigned idx, const PPCContext& ctx) {
  (void)ctx;
  --t_depth;
  {
    ThreadInfo* ti = Info();
    const int d = ti->depth.load(std::memory_order_relaxed) - 1;
    ti->depth.store(d, std::memory_order_relaxed);
    if (d == 0 && g_leave_check) g_leave_check(idx);
  }
  State& s = S();
  if (!s.enabled || idx != s.swap_idx || t_depth != 0) return;
  const uint64_t frame = s.frame.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frame % 120 == 0) {
    std::lock_guard<std::mutex> lock(s.mu);
    for (unsigned i = 0; i < kNumFuncs && i < kMax; ++i) {
      Stat& st = s.stats[i];
      uint64_t c = st.top.load(std::memory_order_relaxed);
      st.per_frame = (c - st.window_base) / 120.0;
      st.window_base = c;
    }
    WriteStats(frame);
    if (!s.seq_written && frame >= s.seq_first + s.seq_count) {
      WriteSeq();
      s.seq_written = true;
      s.seq.clear();
      s.seq.shrink_to_fit();
    }
  }
}

bool SetHandler(uint32_t addr, Handler h) {
  const int i = IndexOf(addr);
  if (i < 0) return false;
  g_handlers[i] = h;
  return true;
}

int IndexOf(uint32_t addr) {
  for (unsigned i = 0; i < kNumFuncs; ++i)
    if (kFuncAddr[i] == addr) return static_cast<int>(i);
  return -1;
}

uint64_t FrameCount() { return g_frames.load(std::memory_order_relaxed); }
void NoteFrame() { g_frames.fetch_add(1, std::memory_order_relaxed); }

}  // namespace q4hle::trace
