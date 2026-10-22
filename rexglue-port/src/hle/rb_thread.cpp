// q4hle - the game's renderer back end on its own thread (default; Q4_RB_THREAD=0 turns it off).
//
// Quake 4 keeps id Tech 4's split between the renderer front end (game thread: visibility,
// interactions, shadow volumes, dynamic models; writes a command list into per-frame memory) and
// the back end (RB_ExecuteBackEndCommands: walks the command list and issues the D3D calls). The
// Xbox 360 build runs the back end inline from R_IssueRenderCommands. Doom 3's SMP design is still
// in the code, switched off: frees of triangle surfaces are deferred to the frame data's list,
// but there is a single frame data buffer.
//
// Here:
//  - R_InitFrameData (0x82291228) creates a second frame data buffer.
//  - R_ToggleSmpFrame (0x82291118, renderSystem vfunc 47) points frameData at the other buffer
//    and recycles it (its deferred tri surfs, its memory blocks, a new command chain).
//    The back end of the previous frame is finished by then (see below), so nothing it used is
//    freed early.
//  - R_IssueRenderCommands (0x82242E48), when called from EndFrame, waits for the worker to finish
//    the previous frame and hands it the new command list; the game thread returns and builds the
//    next frame while the worker runs RB_ExecuteBackEndCommands (and the Swap at its end). Every
//    other caller waits for the worker and runs the back end inline as before.
//  - The worker is a real guest thread (ExCreateThread through the game's CreateThread), entering
//    the async thread procedure (0x822D2610) with a marker parameter.
//  - The vertex cache is shared: its functions are serialised with a lock, and the blocks it
//    would free at EndFrame (idVertexCache's deferred-free list) are held until the worker has
//    finished the frame that might still draw them.
//  - The texture cache's end-of-frame step (0x822E6800) is deferred the same way.
//  - Level loads, model/image purges, world changes and render shutdown wait for the worker.

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <string>
#include <vector>

#include "hle/log.h"
#include "hle/trace.h"

namespace q4tag { void UsePrivateScope(); }
namespace q4rb { extern bool g_active; }  // src/q4_tagscope.cpp (q4_rb_hooks.toml)
namespace q4fe { extern thread_local int32_t t_delta; }  // src/q4_tagscope.cpp
namespace q4hle::rb { const char* ThreadRole(); }  // src/q4_tagscope.cpp

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

uint8_t* g_base = nullptr;

inline uint8_t* P(uint32_t a) { return g_base + a + (a >= 0xE0000000u ? 0x1000u : 0u); }
inline uint32_t Ld(uint32_t a) {
  uint32_t v;
  std::memcpy(&v, P(a), 4);
  return __builtin_bswap32(v);
}
inline void St(uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(P(a), &v, 4);
}

constexpr uint32_t kSmpFrame = 0x8324CBA8;      // tr.smpFrame
constexpr uint32_t kFrameData = 0x82FE7920;     // frameData_t *frameData[2]
constexpr uint32_t kCvarLockSurfaces = 0x829AAC3C;
constexpr uint32_t kCvarSkipBackEnd = 0x829A94E0;
constexpr uint32_t kEndFrame = 0x82244EE8, kEndFrameEnd = 0x82244EE8 + 800;  // renderSystem vfunc 55
constexpr uint32_t kVertexCache = 0x832DD7B0;   // idVertexCache vertexCache
constexpr uint32_t kFrameCount = 0x8324CB98;   // tr.frameCount
constexpr uint32_t kVcListNum = 0x832DD7E4;    // idVertexCache::listNum (frame-temporary list in use)
constexpr uint32_t kVcDeferred = 0x832DD900;    // deferredFreeList sentinel (+0x1c prev, +0x20 next)
constexpr uint32_t kLoadThreadOn = 0x83086343;  // byte: init-time loading-screen thread is drawing
constexpr uint32_t kD3DDevice = 0x832DDA1C;     // the game's D3D device pointer
constexpr uint32_t kHelperMagic = 0x46454A00;   // 'FEJ' + index: front-end helper threads
constexpr uint32_t kWorkerMagic = 0x52425448;   // 'RBTH'

bool Enabled() {
  static const bool on = [] {
    const char* e = std::getenv("Q4_RB_THREAD");
    return !(e && *e == '0');  // on by default; Q4_RB_THREAD=0 keeps the back end on the game thread
  }();
  return on;
}

// ---- worker state ---------------------------------------------------------------------------
std::mutex g_mu;
std::condition_variable g_cv;          // job posted / done
uint32_t g_job = 0;                    // command list for the worker (0 = none)
std::atomic<bool> g_busy{false};       // worker has a job
std::atomic<uint64_t> g_md5r_copies{0};  // MD5R surfaces skinned on a private copy (see Md5rSkin)
std::atomic<uint32_t> g_worker_tid{0};
std::atomic<uint32_t> g_game_tid{0};
uint32_t g_issue_frames = 0;
std::atomic<uint32_t> g_tri_waits{0};   // tri surface mutations that waited for the back end
std::atomic<uint32_t> g_tri_deferred{0};  // direct tri frees turned into deferred ones
std::mutex g_tri_lr_mu;
// Front-end helpers: while they run (g_fe_phase), shared allocators and lists they touch are
// serialised with g_fe_mu (see the end of the file).
std::atomic<bool> g_fe_phase{false};
std::recursive_mutex g_fe_mu;
struct FeLock {
  bool locked;
  FeLock() : locked(g_fe_phase.load(std::memory_order_acquire)) { if (locked) g_fe_mu.lock(); }
  ~FeLock() { if (locked) g_fe_mu.unlock(); }
};
std::unordered_map<uint32_t, uint32_t> g_tri_lr;  // waits by caller           // R_IssueRenderCommands calls (diagnostics)   // thread that issues the render commands
thread_local int t_vc_depth = 0;       // vertex cache lock depth on this thread
thread_local int t_gl_depth = 0;       // GL buffer-state lock depth on this thread (see the end)
bool g_worker_started = false;         // CreateThread called
std::atomic<bool> g_worker_running{false};
bool g_failed = false;                 // could not start: stay synchronous

// vertex cache lock (recursive: Alloc/EndFrame call ActuallyFree etc.)
std::recursive_mutex g_vc_mu;

// work deferred until the worker is idle
std::vector<uint32_t> g_vc_pending;     // vertex cache blocks to ActuallyFree
std::vector<uint32_t> g_texcache_steps; // r3 for 0x822E6800

bool g_in_toggle = false;  // our toggle is freeing a buffer's deferred tri surfs

uint32_t ThreadId() {
#ifdef _WIN32
  return GetCurrentThreadId();
#else
  return 1;
#endif
}

void WaitIdle() {
  if (!g_busy.load(std::memory_order_acquire)) return;
  if (ThreadId() == g_worker_tid.load(std::memory_order_relaxed)) return;
  std::unique_lock<std::mutex> lk(g_mu);
  g_cv.wait(lk, [] { return !g_busy.load(); });
}

// D3D calls from another thread while the worker draws (texture creation, buffer locks from the
// front end, ...): the guest device is not thread safe, so they wait for the worker. Counted and
// the first ones logged, to see how often the overlap is lost.
std::atomic<uint32_t> g_foreign{0}, g_foreign_locked{0};

// The guest D3D device is not thread safe. Every outermost D3D call of the game thread and the
// back-end thread holds g_dev_mu, so calls from the two threads never interleave (the GPU
// interrupt thread is left out, as it always ran alongside the game thread). Resource calls the
// vertex cache makes on the game thread (create/lock/unlock of its buffers, which can insert
// fences into the ring) only take the lock; any other D3D call from the game thread while the
// back end is drawing also waits for the frame to finish, since it may change device state the
// back end relies on (render targets, shaders, streams).
std::mutex g_dev_mu;
thread_local bool t_dev_locked = false;
thread_local uint32_t t_owner_dev = 0;  // device whose owner field this call cleared
thread_local uint32_t t_owner_val = 0;  // and the owner it had

bool UsesDeviceLock() {
  if (!g_worker_running.load(std::memory_order_relaxed)) return false;
  const uint32_t t = ThreadId();
  return t == g_worker_tid.load(std::memory_order_relaxed) || t == g_game_tid.load(std::memory_order_relaxed);
}

void ForeignD3DCheck(unsigned idx, uint32_t lr) {
  if (!UsesDeviceLock()) return;
  const bool game = ThreadId() == g_game_tid.load(std::memory_order_relaxed);
  // Vertex/index buffer create, lock and unlock (0x825E5418-0x825E5670) from the game thread or
  // a front-end helper: run without the device lock. A lock writes cache-flush packets into the
  // command ring only when called on the device's owner thread (the game thread); for the call
  // the owner is cleared, so D3D takes its non-owner path (an atomic range merge) instead.
  // Create and unlock never touch the device.
  static const bool owner_clear = [] {
    const char* e = std::getenv("Q4_RB_OWNER_CLEAR");
    return e && *e == '1';  // off: slowed the back end down (frame waits) in tests
  }();
  if (owner_clear && idx < q4hle::trace::kNumFuncs && game) {
    const uint32_t a = q4hle::trace::kFuncAddr[idx];
    if (a >= 0x825E5418 && a < 0x825E5670) {
      const uint32_t dev = Ld(kD3DDevice);
      if (game && dev && Ld(dev + 0x2888) != 0) {
        t_owner_val = Ld(dev + 0x2888);
        St(dev + 0x2888, 0);
        t_owner_dev = dev;
      }
      return;
    }
  }
  if (game && g_busy.load(std::memory_order_acquire)) {
    const char* name = idx < q4hle::trace::kNumFuncs ? q4hle::trace::kFuncName[idx] : "?";
    if (t_vc_depth > 0 || t_gl_depth > 0) {
      // buffer work under the vertex cache / GL locks: waiting for the frame could deadlock
      g_foreign_locked.fetch_add(1, std::memory_order_relaxed);
    } else {
      if (g_foreign.fetch_add(1) < 40) q4hle::Log("rb thread: D3D %s (lr %08x) from the game thread while drawing: waiting", name, lr);
      WaitIdle();
    }
  }
  g_dev_mu.lock();
  t_dev_locked = true;
}

void D3DLeave(unsigned) {
  if (t_owner_dev) {
    St(t_owner_dev + 0x2888, t_owner_val);
    t_owner_dev = 0;
  }
  if (t_dev_locked) {
    t_dev_locked = false;
    g_dev_mu.unlock();
  }
}
}  // namespace

// guest functions we call
REX_EXTERN(sub_82285BF0);  // RB_ExecuteBackEndCommands(cmds)
REX_EXTERN(sub_82243018);  // start a new command chain in frameData[smpFrame]
REX_EXTERN(sub_822A40B8);  // R_FreeDeferredTriSurfs(frameData)
REX_EXTERN(sub_82291350);  // R_CountFrameData (high-water mark of frameData[smpFrame])
REX_EXTERN(sub_82854B18);  // CreateThread(attr, stack, start, param, flags, tid*)
REX_EXTERN(__imp__sub_822A7968);  // idVertexCache::ActuallyFree(this, block)
REX_EXTERN(__imp__sub_822E6800);  // texture cache end-of-frame step

namespace {

// Runs deferred end-of-frame work. Caller: game thread, worker idle.
void FlushDeferredImpl(PPCContext& ctx, uint8_t* base);
// (keeps the caller's registers: callers pass their own ctx and then run the original function)
void FlushDeferred(PPCContext& ctx, uint8_t* base) {
  // only from the game thread with the back end idle: the held frees may still be in use by the
  // frame the back end is drawing (the sync hooks can also be reached from the back end itself,
  // e.g. an image purge during a bind)
  if (ThreadId() == g_worker_tid.load(std::memory_order_relaxed) || g_busy.load()) return;
  if (g_vc_pending.empty() && g_texcache_steps.empty()) return;
  const PPCContext saved = ctx;
  FlushDeferredImpl(ctx, base);
  ctx = saved;
}
void FlushDeferredImpl(PPCContext& ctx, uint8_t* base) {
  if (!g_vc_pending.empty()) {
    std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);
    for (uint32_t blk : g_vc_pending) {
      // put the block back on the deferred-free list (tail) and free it properly
      const uint32_t tail = Ld(kVcDeferred + 0x1C);
      St(blk + 0x1C, tail);
      St(blk + 0x20, kVcDeferred);
      St(tail + 0x20, blk);
      St(kVcDeferred + 0x1C, blk);
      ctx.r3.u64 = kVertexCache;
      ctx.r4.u64 = blk;
      __imp__sub_822A7968(ctx, base);
    }
    g_vc_pending.clear();
  }
  for (uint32_t tc : g_texcache_steps) {
    ctx.r3.u64 = tc;
    __imp__sub_822E6800(ctx, base);
  }
  g_texcache_steps.clear();
}

void StartWorker(PPCContext& ctx, uint8_t* base) {
  g_worker_started = true;
  const PPCContext saved = ctx;
  ctx.r3.u64 = 0;              // attributes
  ctx.r4.u64 = 0x100000;       // 1 MB guest stack
  ctx.r5.u64 = 0x822D2610;     // async thread procedure (our override recognises the marker)
  ctx.r6.u64 = kWorkerMagic;   // parameter
  ctx.r7.u64 = 0;              // flags: run now
  ctx.r8.u64 = 0;              // thread id: not needed
  sub_82854B18(ctx, base);
  const uint32_t handle = ctx.r3.u32;
  ctx = saved;
  if (!handle) {
    g_failed = true;
    q4hle::Log("rb thread: CreateThread failed, back end stays on the game thread");
    return;
  }
  // wait until the worker has started (it signals through g_worker_running)
  std::unique_lock<std::mutex> lk(g_mu);
  g_cv.wait_for(lk, std::chrono::seconds(5), [] { return g_worker_running.load(); });
  if (!g_worker_running) {
    g_failed = true;
    q4hle::Log("rb thread: worker did not start, back end stays on the game thread");
  } else {
    q4hle::Log("rb thread: back end running on its own thread");
  }
}

uint64_t g_frames = 0;
std::atomic<uint64_t> g_frame_waits{0};

void WorkerMain(PPCContext& ctx, uint8_t* base) {
  // the game's memory-tag scope stack is one global; this thread gets its own (q4_tagscope.cpp)
  q4tag::UsePrivateScope();
  g_worker_tid = ThreadId();
  q4rb::g_active = true;
  q4hle::trace::g_leave_check = D3DLeave;
  q4hle::trace::g_enter_check = ForeignD3DCheck;
  {
    std::lock_guard<std::mutex> lk(g_mu);
    g_worker_running = true;
  }
  g_cv.notify_all();
  for (;;) {
    uint32_t job;
    {
      std::unique_lock<std::mutex> lk(g_mu);
      g_cv.wait(lk, [] { return g_job != 0; });
      job = g_job;
    }
    ctx.r3.u64 = job;
    sub_82285BF0(ctx, base);
    {
      std::lock_guard<std::mutex> lk(g_mu);
      g_job = 0;
      g_busy.store(false, std::memory_order_release);
    }
    g_cv.notify_all();
    if (g_frames % 3600 == 3599) {
      std::lock_guard<std::mutex> lk(g_tri_lr_mu);
      std::string top;
      for (auto& [k, v] : g_tri_lr) { char b[32]; std::snprintf(b, sizeof b, " %08x:%u", k, v); top += b; }
      if (!top.empty()) q4hle::Log("rb thread: tri waits by caller:%s", top.c_str());
    }
    if (++g_frames % 3600 == 0)
      q4hle::Log("rb thread: %llu frames, %u game-thread D3D waits, %u tri waits, %u tri frees deferred, %u vertex cache D3D calls during drawing, %llu frame waits, %llu skinned-surface copies",
                 (unsigned long long)g_frames, g_foreign.load(), g_tri_waits.load(), g_tri_deferred.load(), g_foreign_locked.load(), (unsigned long long)g_frame_waits.load(),
                 (unsigned long long)g_md5r_copies.load());
  }
}

}  // namespace

void HelperMain(PPCContext& ctx, uint8_t* base, uint32_t index);  // front-end helpers (end of file)
bool IsHelperThread();

// ---- async thread procedure: worker entry when given the marker --------------------------------
REX_EXTERN(__imp__sub_822D2610);
extern "C" REX_FUNC(sub_822D2610) {
  if (ctx.r3.u32 == kWorkerMagic) {
    g_base = base;
    WorkerMain(ctx, base);
    return;
  }
  if ((ctx.r3.u32 & 0xFFFFFF00u) == kHelperMagic) {
    g_base = base;
    HelperMain(ctx, base, ctx.r3.u32 & 0xFF);
    return;
  }
  __imp__sub_822D2610(ctx, base);
}

// ---- frame data ---------------------------------------------------------------------------------
// The game reads frame memory through `frameData` (0x82FE7920; it indexes it by smpFrame, which
// stays 0 - the word after it is an unrelated counter). Two buffers are kept here and the pointer
// is swapped between them at each ToggleSmpFrame.
namespace {
uint32_t g_fd[2] = {0, 0};
uint32_t g_fd_cur = 0;
}

// R_ShutdownFrameData (0x822911B0): frees `frameData`; free the other buffer too
REX_EXTERN(__imp__sub_822911B0);
extern "C" REX_FUNC(sub_822911B0) {
  g_base = base;
  if (!Enabled()) return __imp__sub_822911B0(ctx, base);
  WaitIdle();
  FlushDeferred(ctx, base);
  const uint32_t cur = Ld(kFrameData);
  __imp__sub_822911B0(ctx, base);
  for (uint32_t& fd : g_fd) {
    if (fd && fd != cur) {
      St(kFrameData, fd);
      __imp__sub_822911B0(ctx, base);
    }
    fd = 0;
  }
  St(kFrameData, 0);
  g_fd_cur = 0;
}

// R_InitFrameData (0x82291228): frees the old frame data (through the override above), allocates
// `frameData` and resets it. Run twice; the first buffer is set aside so the second run allocates
// the other one.
REX_EXTERN(__imp__sub_82291228);
extern "C" REX_FUNC(sub_82291228) {
  g_base = base;
  if (!Enabled()) return __imp__sub_82291228(ctx, base);
  WaitIdle();
  const PPCContext saved = ctx;
  __imp__sub_82291228(ctx, base);
  const uint32_t first = Ld(kFrameData);
  St(kFrameData, 0);
  ctx = saved;
  __imp__sub_82291228(ctx, base);
  const uint32_t second = Ld(kFrameData);
  g_fd[0] = first;
  g_fd[1] = second;
  g_fd_cur = 0;
  St(kFrameData, first);
  q4hle::Log("rb thread: frame data %08x / %08x", first, second);
}

// R_ToggleSmpFrame (0x82291118, renderSystem vfunc 47): switch `frameData` to the other buffer,
// then let the original recycle it (deferred tri surf frees, memory blocks, new command chain).
// The back end that last used that buffer has finished: R_IssueRenderCommands waited for it.
REX_EXTERN(__imp__sub_82291118);
extern "C" REX_FUNC(sub_82291118) {
  g_base = base;
  if (Enabled() && g_fd[1] && Ld(Ld(kCvarLockSurfaces) + 0x24) == 0) {
    g_fd_cur ^= 1u;
    St(kFrameData, g_fd[g_fd_cur]);
  }
  g_in_toggle = true;
  __imp__sub_82291118(ctx, base);
  g_in_toggle = false;
}

// ---- R_IssueRenderCommands --------------------------------------------------------------------
REX_EXTERN(__imp__sub_82242E48);
extern "C" REX_FUNC(sub_82242E48) {
  g_base = base;
  if (!Enabled()) return __imp__sub_82242E48(ctx, base);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  g_game_tid.store(ThreadId(), std::memory_order_relaxed);
  const uint32_t fd = Ld(kFrameData + 4 * Ld(kSmpFrame));  // smpFrame is always 0
  const uint32_t cmd = Ld(fd + 0x14);
  if (Ld(cmd) == 0 && Ld(cmd + 4) == 0) return;
  if (Ld(Ld(kCvarSkipBackEnd) + 0x24) == 0) {
    if (g_busy.load()) g_frame_waits.fetch_add(1, std::memory_order_relaxed);
    WaitIdle();
    FlushDeferred(ctx, base);
    // not while the init-time loading-screen thread (0x822B39B8) draws with D3D itself
    const bool async = lr >= kEndFrame && lr < kEndFrameEnd && g_fd[1] != 0 && *P(kLoadThreadOn) == 0;
    if (async && !g_worker_started) StartWorker(ctx, base);
    ++g_issue_frames;
    if (async && g_worker_running && !g_failed) {
      {
        std::lock_guard<std::mutex> lk(g_mu);
        g_job = cmd;
        g_busy.store(true, std::memory_order_release);
      }
      g_cv.notify_all();
    } else {
      ctx.r3.u64 = cmd;
      sub_82285BF0(ctx, base);
    }
  }
  sub_82243018(ctx, base);
}

// ---- texture cache end-of-frame step: after the worker is done ---------------------------------
extern "C" REX_FUNC(sub_822E6800) {
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  if (Enabled() && g_worker_running && lr >= kEndFrame && lr < kEndFrameEnd) {
    g_texcache_steps.push_back(ctx.r3.u32);
    return;
  }
  __imp__sub_822E6800(ctx, base);
}

// ---- vertex cache: serialised; EndFrame's frees held until the worker is idle ------------------
#define Q4_VC_LOCKED(addr)                                          \
  REX_EXTERN(__imp__sub_##addr);                                    \
  extern "C" REX_FUNC(sub_##addr) {                                 \
    if (!Enabled()) return __imp__sub_##addr(ctx, base);            \
    std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);             \
    ++t_vc_depth;                                                   \
    __imp__sub_##addr(ctx, base);                                   \
    --t_vc_depth;                                                   \
  }
Q4_VC_LOCKED(822A7D08)  // Alloc
Q4_VC_LOCKED(822A8010)  // Touch
#undef Q4_VC_LOCKED

REX_EXTERN(__imp__sub_822A8100);
extern "C" REX_FUNC(sub_822A8100) {  // Free
  if (!Enabled()) return __imp__sub_822A8100(ctx, base);
  std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);
  ++t_vc_depth;
  __imp__sub_822A8100(ctx, base);
  --t_vc_depth;
}

REX_EXTERN(__imp__sub_822A7A90);
extern "C" REX_FUNC(sub_822A7A90) {  // Position
  if (!Enabled()) return __imp__sub_822A7A90(ctx, base);
  std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);
  const uint32_t b = ctx.r4.u32;
  if (b == 0 || Ld(b + 0x14) == 0) {
    q4hle::Log("rb thread: bad vertex cache block %08x (tag %u) in Position from %08x on %s, issue frame %u",
               b, b ? Ld(b + 0x14) : 0, uint32_t(ctx.lr), q4hle::rb::ThreadRole(), g_issue_frames);
  }
  ++t_vc_depth;
  __imp__sub_822A7A90(ctx, base);
  --t_vc_depth;
}

extern "C" REX_FUNC(sub_822A7968) {  // ActuallyFree
  if (!Enabled()) return __imp__sub_822A7968(ctx, base);
  std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);

  ++t_vc_depth;
  __imp__sub_822A7968(ctx, base);
  --t_vc_depth;
}

REX_EXTERN(__imp__sub_822A81D8);
extern "C" REX_FUNC(sub_822A81D8) {  // EndFrame
  g_base = base;
  if (!Enabled()) return __imp__sub_822A81D8(ctx, base);
  std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);
  if (g_worker_running) {
    // detach the deferred-free list: the worker may still draw these blocks this frame
    for (uint32_t b = Ld(kVcDeferred + 0x20); b && b != kVcDeferred; b = Ld(b + 0x20)) g_vc_pending.push_back(b);
    St(kVcDeferred + 0x1C, kVcDeferred);
    St(kVcDeferred + 0x20, kVcDeferred);
  }
  // The original recycles the frame-temporary list number (tr.frameCount & 1). That only
  // alternates if frameCount advances by exactly one per frame; when it advances by two (extra
  // views), it would recycle the list the back end is drawing from right now. The original runs
  // with frameCount's parity set so it always picks the other list than the current one.
  const uint32_t fc = Ld(kFrameCount);
  if (g_worker_running) St(kFrameCount, (fc & ~1u) | ((Ld(kVcListNum) & 1u) ^ 1u));
  ++t_vc_depth;
  __imp__sub_822A81D8(ctx, base);
  --t_vc_depth;
  if (g_worker_running) St(kFrameCount, fc);
}

// ---- places that must not run while the worker draws ---------------------------------------------
#define Q4_RB_SYNC(addr)                                 \
  REX_EXTERN(__imp__sub_##addr);                         \
  extern "C" REX_FUNC(sub_##addr) {                      \
    if (Enabled() && g_worker_running) {                 \
      WaitIdle();                                        \
      FlushDeferred(ctx, base);                          \
    }                                                    \
    __imp__sub_##addr(ctx, base);                        \
  }
Q4_RB_SYNC(82240120)  // idRenderModelManagerLocal::BeginLevelLoad
Q4_RB_SYNC(822401F0)  // idRenderModelManagerLocal::EndLevelLoad
Q4_RB_SYNC(8223EE80)  // idRenderModelManager::Reset
Q4_RB_SYNC(8223EDD0)  // idRenderModelManagerLocal::Shutdown
Q4_RB_SYNC(82207B40)  // idImageManager::EndLevelLoad
Q4_RB_SYNC(8220C6F8)  // idImage::PurgeImage
Q4_RB_SYNC(82203870)  // R_ReloadImages_f
Q4_RB_SYNC(82259F58)  // idRenderWorldLocal::FreeWorld
Q4_RB_SYNC(8225B7F8)  // idRenderWorldLocal::InitFromMap
Q4_RB_SYNC(8224BC88)  // idRenderSystemLocal::Shutdown
Q4_RB_SYNC(821B3418)  // idSessionLocal::ExecuteMapChange
Q4_RB_SYNC(822B39C8)  // start the init-time loading-screen thread (draws with D3D directly)
#undef Q4_RB_SYNC

// R_FreeDeferredTriSurfs from anywhere but our toggle (model manager reset / level load)
REX_EXTERN(__imp__sub_822A40B8);
namespace { thread_local bool t_in_deferred_free = false; }
extern "C" REX_FUNC(sub_822A40B8) {
  if (Enabled() && g_worker_running && !g_in_toggle) WaitIdle();
  t_in_deferred_free = true;
  __imp__sub_822A40B8(ctx, base);
  t_in_deferred_free = false;
}

REX_EXTERN(sub_8234FB98);  // Mem_Alloc16(size, tag)
REX_EXTERN(sub_8234FC18);  // Mem_Free16(ptr)
namespace {
void Md5rForget(PPCContext& ctx, uint8_t* base, uint32_t tri);
}

// Triangle surfaces are shared between the frame being built and the frame being drawn. Two
// game-thread paths change them in place: freeing a surface outright (R_ReallyFreeStaticTriSurf,
// outside the deferred-free list) and dropping a model's vertex caches (idRenderModelStatic::
// FreeVertexCache, which clears tri->ambientCache / shadowCache). Both wait for the back end.
namespace {
void TriMutationWait(const char* what, uint32_t lr) {
  if (!Enabled() || !g_worker_running.load(std::memory_order_relaxed) || t_in_deferred_free) return;
  if (ThreadId() == g_worker_tid.load(std::memory_order_relaxed)) return;
  if (!g_busy.load() || t_vc_depth > 0 || t_gl_depth > 0) return;
  if (g_tri_waits.fetch_add(1) < 30) q4hle::Log("rb thread: %s (lr %08x) while drawing: waiting", what, lr);
  {
    std::lock_guard<std::mutex> lk(g_tri_lr_mu);
    ++g_tri_lr[lr];
  }
  WaitIdle();
}
}  // namespace

REX_EXTERN(__imp__sub_8223C368);
extern "C" REX_FUNC(sub_8223C368) {  // R_ReallyFreeStaticTriSurf
  TriMutationWait("free tri surface", uint32_t(ctx.lr));
  Md5rForget(ctx, base, ctx.r3.u32);
  __imp__sub_8223C368(ctx, base);
}

// R_ReallyFreeStaticTriSurf (0x822A3E18) called directly, outside the deferred-free list (the
// interaction code drops surfaces it has just built, R_FreeStaticTriSurf's immediate path): while
// the back end draws, put the surface on the current frame data's deferred list instead, the
// way R_FreeStaticTriSurf defers. It is then freed two frames later, after the back end is done.
REX_EXTERN(__imp__sub_822A3E18);
extern "C" REX_FUNC(sub_822A3E18) {
  FeLock fl;
  const uint32_t tri = ctx.r3.u32;
  if (Enabled() && tri && !t_in_deferred_free && g_worker_running.load(std::memory_order_relaxed) &&
      ThreadId() != g_worker_tid.load(std::memory_order_relaxed)) {
    const uint32_t fd = Ld(kFrameData);
    if (fd) {
      g_tri_deferred.fetch_add(1, std::memory_order_relaxed);
      St(tri + 0x74, 0);
      if (Ld(fd + 0xC) == 0) {
        St(fd + 8, tri);
      } else {
        St(Ld(fd + 0xC) + 0x74, tri);
      }
      St(fd + 0xC, tri);
      return;
    }
  }
  Md5rForget(ctx, base, tri);
  __imp__sub_822A3E18(ctx, base);
}

REX_EXTERN(__imp__sub_822A3D60);
extern "C" REX_FUNC(sub_822A3D60) {  // R_FreeStaticTriSurfVertexCaches (also clears the MD5R mesh link)
  // The MD5R skinning update (0x82265000) calls this every frame on the surface it re-skins and
  // sets the mesh link (+0x88) and its offset (+0x54) again right after. For such a surface
  // (mesh link set, no shadow caches) the call only clears those two fields, which would leave
  // the back end a moment where the surface it is drawing has no mesh: skip it there.
  const uint32_t tri = ctx.r3.u32;
  const uint32_t lr = uint32_t(ctx.lr);
  if (Enabled() && g_worker_running.load(std::memory_order_relaxed) && lr >= 0x82265000 && lr < 0x82265000 + 820 &&
      tri && Ld(tri + 0x88) != 0 && Ld(tri + 0x78) == 0 &&
      !(Ld(tri + 0x84) != 0 && (Ld(tri + 0x6C) != 0 || Ld(tri + 0x28) != 0)))
    return;
  TriMutationWait("free tri vertex caches", lr);
  __imp__sub_822A3D60(ctx, base);
}

// MD5R skinning (0x82265000, and 0x82265338 for the unskinned variant) re-skins a model surface
// every frame *in place*: it resets the surface's bounds and fields, then rewrites them and its
// joint-matrix buffer (+0x8C/+0x90, count at +0x94). The back end draws the previous frame from
// the same surface at the same time, so it could see half-written bounds or matrices (seen as the
// view weapon flickering unlit). While the back end runs, the update instead works on a private
// copy of the surface, writing the matrices into a second buffer (two per surface, alternating),
// and the finished copy is put back in one step. Q4_RB_MD5R=wait waits for the back end instead;
// Q4_RB_MD5R=0 turns this off.
namespace {
int Md5rMode() {  // 0 off, 1 wait, 2 copy
  static const int mode = [] {
    const char* e = std::getenv("Q4_RB_MD5R");
    if (!e || !*e) return 2;
    if (std::strcmp(e, "wait") == 0) return 1;
    if (*e == '0') return 0;
    return 2;
  }();
  return mode;
}

constexpr uint32_t kTriSize = 0xB4;
struct AltJoints {
  uint32_t ptr = 0, count = 0;
};
std::mutex g_md5r_mu;
std::unordered_map<uint32_t, AltJoints> g_md5r_alt;  // surface -> its other joint buffer
thread_local uint32_t t_md5r_scratch = 0;

uint32_t GuestAlloc16(PPCContext& ctx, uint8_t* base, uint32_t size) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = size;
  ctx.r4.u64 = 0x11;
  sub_8234FB98(ctx, base);
  const uint32_t p = ctx.r3.u32;
  ctx = saved;
  return p;
}
void GuestFree16(PPCContext& ctx, uint8_t* base, uint32_t p) {
  if (!p) return;
  const PPCContext saved = ctx;
  ctx.r3.u64 = p;
  sub_8234FC18(ctx, base);
  ctx = saved;
}

// A surface is being freed: drop its second joint buffer (the back end is done with it: real
// frees happen from the deferred list or while the back end is idle).
void Md5rForget(PPCContext& ctx, uint8_t* base, uint32_t tri) {
  uint32_t alt = 0;
  {
    std::lock_guard<std::mutex> lk(g_md5r_mu);
    auto it = g_md5r_alt.find(tri);
    if (it == g_md5r_alt.end()) return;
    alt = it->second.ptr;
    g_md5r_alt.erase(it);
  }
  GuestFree16(ctx, base, alt);
}

template <void (*Orig)(PPCContext&, uint8_t*), bool kJoints>
void Md5rSkin(PPCContext& ctx, uint8_t* base, const char* what) {
  const int mode = Md5rMode();
  if (mode == 0 || !Enabled() || !g_worker_running.load(std::memory_order_relaxed) ||
      ThreadId() == g_worker_tid.load(std::memory_order_relaxed))
    return Orig(ctx, base);
  if (mode == 1) {
    TriMutationWait(what, uint32_t(ctx.lr));
    return Orig(ctx, base);
  }
  const uint32_t src = ctx.r3.u32, surf = ctx.r4.u32;
  const uint32_t tri = surf ? Ld(surf + 8) : 0;
  // Only the in-place path; the others allocate a new surface (the old one is freed deferred).
  const bool in_place = tri && (Ld(tri + 0x28) != 0 || (Ld(tri + 0x2C) != 0 && Ld(tri + 0x24) == Ld(src + 0x2C) &&
                                                         Ld(tri + 0x34) == Ld(src + 0x30)));
  if (!in_place) return Orig(ctx, base);
  if (!t_md5r_scratch) t_md5r_scratch = GuestAlloc16(ctx, base, 0xC0);
  const uint32_t scratch = t_md5r_scratch;
  if (!scratch) return Orig(ctx, base);
  std::memcpy(P(scratch), P(tri), kTriSize);
  if (kJoints) {
    const uint32_t cur = Ld(tri + 0x90), count = Ld(tri + 0x94);
    if (cur && count && count < 0x10000) {
      uint32_t alt = 0, free_old = 0;
      {
        std::lock_guard<std::mutex> lk(g_md5r_mu);
        AltJoints& a = g_md5r_alt[tri];
        if (a.ptr && a.count < count) {
          free_old = a.ptr;
          a.ptr = 0;
        }
        alt = a.ptr;
      }
      GuestFree16(ctx, base, free_old);
      if (!alt) alt = GuestAlloc16(ctx, base, count * 0x40);
      if (alt) {
        St(scratch + 0x8C, alt);
        St(scratch + 0x90, alt);
        std::lock_guard<std::mutex> lk(g_md5r_mu);
        g_md5r_alt[tri] = AltJoints{cur, count};  // the buffer the back end may be drawing with
      }
    }
  }
  St(surf + 8, scratch);
  Orig(ctx, base);
  if (Ld(surf + 8) == scratch) {
    std::memcpy(P(tri), P(scratch), kTriSize);
    St(surf + 8, tri);
    g_md5r_copies.fetch_add(1, std::memory_order_relaxed);
  } else {
    q4hle::Log("rb thread: %s replaced the surface unexpectedly", what);
    t_md5r_scratch = 0;  // now owned by the game
  }
}
}  // namespace

REX_EXTERN(__imp__sub_82265000);
extern "C" REX_FUNC(sub_82265000) { Md5rSkin<__imp__sub_82265000, true>(ctx, base, "md5r skin"); }
REX_EXTERN(__imp__sub_82265338);
extern "C" REX_FUNC(sub_82265338) { Md5rSkin<__imp__sub_82265338, false>(ctx, base, "md5r skin (static)"); }

REX_EXTERN(__imp__sub_82225158);
extern "C" REX_FUNC(sub_82225158) {  // idRenderModelStatic::FreeVertexCache
  TriMutationWait("free model vertex caches", uint32_t(ctx.lr));
  __imp__sub_82225158(ctx, base);
}

namespace q4hle::rb {
const char* ThreadRole() {
  const uint32_t t = ThreadId();
  if (t == g_worker_tid.load()) return "back-end thread";
  if (t == g_game_tid.load()) return "game thread";
  if (IsHelperThread()) return "front-end helper";
  return "other";
}
}  // namespace q4hle::rb

// ---- GL emulation buffer state ---------------------------------------------------------------
// The game's GL layer over D3D keeps the bound array / element buffer in two globals
// (0x832DDAB0 / 0x832DDAB4) and a shared buffer-object table and resource-header lists. The front
// end binds, fills and maps buffers (vertex cache, model buffers) while the back end binds and
// draws. The functions that use that state are serialised by g_gl_mu, and on the game thread they
// run with the game thread's own pair of bindings swapped in, so a bind on one thread never
// changes what the other thread draws with.
namespace {
constexpr uint32_t kGlArrayBuf = 0x832DDAB0, kGlElementBuf = 0x832DDAB4;
constexpr uint32_t kGlHdrFree = 0x8330DAE0, kGlHdrPending = 0x8330DAE4;
std::recursive_mutex g_gl_mu;
thread_local uint32_t t_ab0 = 0, t_ab4 = 0;
thread_local bool t_bind_init = false;

template <void (*Orig)(PPCContext&, uint8_t*)>
void GlBufferCall(PPCContext& ctx, uint8_t* base) {
  if (!Enabled() || !g_worker_running.load(std::memory_order_relaxed)) return Orig(ctx, base);
  std::lock_guard<std::recursive_mutex> lk(g_gl_mu);
  const bool game = ThreadId() != g_worker_tid.load(std::memory_order_relaxed);
  if (!game || t_gl_depth > 0) {
    ++t_gl_depth;
    Orig(ctx, base);
    --t_gl_depth;
    return;
  }
  // each thread other than the back end (game thread, front-end helpers) has its own bindings
  if (!t_bind_init) {
    t_ab0 = Ld(kGlArrayBuf);
    t_ab4 = Ld(kGlElementBuf);
    t_bind_init = true;
  }
  const uint32_t s0 = Ld(kGlArrayBuf), s4 = Ld(kGlElementBuf);
  St(kGlArrayBuf, t_ab0);
  St(kGlElementBuf, t_ab4);
  ++t_gl_depth;
  Orig(ctx, base);
  --t_gl_depth;
  t_ab0 = Ld(kGlArrayBuf);
  t_ab4 = Ld(kGlElementBuf);
  St(kGlArrayBuf, s0);
  St(kGlElementBuf, s4);
}
}  // namespace

#define Q4_GL_BUF(addr)                                  \
  REX_EXTERN(__imp__sub_##addr);                         \
  extern "C" REX_FUNC(sub_##addr) { GlBufferCall<__imp__sub_##addr>(ctx, base); }
Q4_GL_BUF(822E2438)  // glBindBuffer
Q4_GL_BUF(822E2528)  // glDeleteBuffers
Q4_GL_BUF(822E2608)  // glGenBuffers
Q4_GL_BUF(822E28E0)  // glBufferData
Q4_GL_BUF(822E2AF8)  // glBufferSubData
Q4_GL_BUF(822E2C00)  // glMapBuffer
Q4_GL_BUF(822E2CA8)  // glUnmapBuffer
Q4_GL_BUF(82267970)  // draws that read the bound buffers
Q4_GL_BUF(822DE140)
Q4_GL_BUF(822DE740)
Q4_GL_BUF(822E1648)
#undef Q4_GL_BUF

// Swap command (0x822B4238): after the D3D Swap it releases orphaned buffer headers whose GPU
// use has finished. That list is shared with glBufferData, so the original runs with the list
// detached and the release loop (same logic) runs here under g_gl_mu.
REX_EXTERN(__imp__sub_822B4238);
REX_EXTERN(sub_825DF2D8);  // resource busy?
REX_EXTERN(sub_825E5330);  // release resource
extern "C" REX_FUNC(sub_822B4238) {
  if (!Enabled() || !g_worker_running.load(std::memory_order_relaxed)) return __imp__sub_822B4238(ctx, base);
  uint32_t detached;
  {
    std::lock_guard<std::recursive_mutex> lk(g_gl_mu);
    detached = Ld(kGlHdrPending);
    St(kGlHdrPending, 0);
  }
  __imp__sub_822B4238(ctx, base);
  std::lock_guard<std::recursive_mutex> lk(g_gl_mu);
  if (detached) {
    // put the detached chain back in front of what was added meanwhile
    uint32_t tail = detached;
    while (Ld(tail + 4)) tail = Ld(tail + 4);
    St(tail + 4, Ld(kGlHdrPending));
    St(kGlHdrPending, detached);
  }
  const PPCContext saved = ctx;
  uint32_t link = kGlHdrPending;
  for (uint32_t p = Ld(link); p;) {
    const uint32_t next = Ld(p + 4);
    ctx.r3.u64 = Ld(p);
    sub_825DF2D8(ctx, base);
    if (ctx.r3.u32 == 0) {
      ctx.r3.u64 = Ld(p);
      sub_825E5330(ctx, base);
      St(p + 4, Ld(kGlHdrFree));
      St(kGlHdrFree, p);
      St(link, next);
    } else {
      link = p + 4;
    }
    p = next;
  }
  ctx = saved;
}

// idVertexCache::PurgeAll (map change, vid_restart, back-end renderer change): frees every
// static block at once, so the back end must have finished first.
REX_EXTERN(__imp__sub_822A7CA8);
extern "C" REX_FUNC(sub_822A7CA8) {
  if (Enabled() && g_worker_running) {
    WaitIdle();
    FlushDeferred(ctx, base);
  }
  if (!Enabled()) return __imp__sub_822A7CA8(ctx, base);
  std::lock_guard<std::recursive_mutex> vlk(g_vc_mu);
  ++t_vc_depth;
  __imp__sub_822A7CA8(ctx, base);
  --t_vc_depth;
}


// ==== front-end helper threads ===================================================================
// The heaviest step of building a frame's scene is idInteraction::CreateInteraction (0x822128F8):
// for each entity/light pair that has no surfaces yet (new, or the entity's animated model
// changed) it clips the model's triangles to the light and builds the shadow volumes. Inside
// R_AddLightSurfaces' loop (0x8228DCC0) this runs one interaction at a time on the game thread.
//
// Here, before that loop, the game thread finds the interactions it is about to create (same
// tests as idInteraction::AddActiveInteraction: light seen this view, entity model surfaces,
// stale surfaces freed when the model changed, light-volume culling) and creates them on a pool
// of guest helper threads plus itself, one entity per job. The original loop then finds them
// created and only adds the draw surfaces.
//
// What makes CreateInteraction safe to run on several threads:
//  - shadow volume working state (static variables 0x82FE7928-0x8300FA00) is per thread:
//    q4_rb_hooks.toml rebases its base registers onto each helper's own copy;
//  - the memory-tag scope is per thread (q4_tagscope.cpp), GL buffer bindings per thread;
//  - frame memory, the triangle-surface header pool, deferred frees and the interaction lists of
//    lights are serialised (FeLock); the vertex cache and heap have their own locks;
//  - one entity per job, so an entity's interactions are never created on two threads at once;
//  - buffer locks from helpers take D3D's non-owner path (the back-end thread owns the device).
namespace {

constexpr uint32_t kViewDef = 0x8325CD60;      // tr.viewDef
constexpr uint32_t kViewCount = 0x8324CB9C;    // tr.viewCount
constexpr uint32_t kShadowLo = 0x82FE7928, kShadowHi = 0x8300FA00;  // shadow code statics
constexpr int kMaxHelpers = 3;

bool FeEnabled() {
  static const bool on = [] {
    // experimental, off by default: no measurable gain on the test replays (the helpers mostly
    // queue on the shared allocators and locks), and MD5R mesh data shared between entities of
    // one model is still built lazily without a lock in places
    const char* e = std::getenv("Q4_FE_JOBS");
    return Enabled() && e && *e == '1';
  }();
  return on;
}

struct FeJob { uint32_t model; uint32_t first; uint32_t count; };  // interactions in g_fe_inters
std::vector<std::vector<FeJob>> g_fe_groups;  // jobs (one entity each)
std::vector<uint32_t> g_fe_inters;
std::atomic<uint32_t> g_fe_next{0};
std::atomic<uint32_t> g_fe_done_helpers{0};
std::mutex g_fe_mu2;
std::condition_variable g_fe_cv;
uint64_t g_fe_gen = 0;                 // job batch number (under g_fe_mu2)
int g_fe_helpers = 0;                  // started helpers
bool g_fe_started = false;
int32_t g_fe_delta[kMaxHelpers] = {};
std::atomic<uint32_t> g_fe_tids[kMaxHelpers] = {};
uint64_t g_fe_batches = 0, g_fe_created = 0;

// host function for a guest address (virtual calls)
using GuestFnPtr = void (*)(PPCContext&, uint8_t*);
GuestFnPtr GuestFn(uint32_t addr) {
  static std::unordered_map<uint32_t, GuestFnPtr> map = [] {
    std::unordered_map<uint32_t, GuestFnPtr> m;
    for (const PPCFuncMapping* f = PPCFuncMappings; f->host; ++f) m.emplace(uint32_t(f->guest), f->host);
    return m;
  }();
  auto it = map.find(addr);
  return it == map.end() ? nullptr : it->second;
}
uint32_t CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot) {
  GuestFnPtr fn = GuestFn(Ld(Ld(obj) + slot));
  if (!fn) return 0;
  ctx.r3.u64 = obj;
  fn(ctx, base);
  return ctx.r3.u32;
}

}  // namespace

REX_EXTERN(__imp__sub_822128F8);  // idInteraction::CreateInteraction(inter, model)
REX_EXTERN(sub_8228CA18);         // R_EntityDefDynamicModel(entityDef, ?)
REX_EXTERN(sub_82212028);         // interaction: light shadows/scissor test
REX_EXTERN(sub_82212558);         // interaction: culled by the light volume?
REX_EXTERN(sub_82211D38);         // interaction: free surfaces

namespace {
void RunFeJobs(PPCContext& ctx, uint8_t* base) {
  for (;;) {
    const uint32_t j = g_fe_next.fetch_add(1, std::memory_order_relaxed);
    if (j >= g_fe_groups.size()) break;
    for (const FeJob& job : g_fe_groups[j]) {
      for (uint32_t k = 0; k < job.count; ++k) {
        ctx.r3.u64 = g_fe_inters[job.first + k];
        ctx.r4.u64 = job.model;
        __imp__sub_822128F8(ctx, base);
      }
    }
  }
}
}  // namespace

bool IsHelperThread() {
  const uint32_t t = ThreadId();
  for (auto& h : g_fe_tids)
    if (h.load(std::memory_order_relaxed) == t) return true;
  return false;
}

void HelperMain(PPCContext& ctx, uint8_t* base, uint32_t index) {
  q4tag::UsePrivateScope();
  if (index < kMaxHelpers) {
    q4fe::t_delta = g_fe_delta[index];
    g_fe_tids[index] = ThreadId();
  }
  uint64_t seen = 0;
  {
    std::lock_guard<std::mutex> lk(g_fe_mu2);
    seen = g_fe_gen;
    ++g_fe_helpers;
  }
  g_fe_cv.notify_all();
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_fe_mu2);
      g_fe_cv.wait(lk, [&] { return g_fe_gen != seen; });
      seen = g_fe_gen;
    }
    RunFeJobs(ctx, base);
    if (g_fe_done_helpers.fetch_add(1) + 1 == uint32_t(g_fe_helpers)) {
      std::lock_guard<std::mutex> lk(g_fe_mu2);
      g_fe_cv.notify_all();
    }
  }
}

namespace {
void StartFeHelpers(PPCContext& ctx, uint8_t* base) {
  g_fe_started = true;
  const PPCContext saved = ctx;
  const uint32_t size = kShadowHi - kShadowLo;
  int want = kMaxHelpers;
  if (const char* e = std::getenv("Q4_FE_THREADS")) want = std::max(0, std::min(kMaxHelpers, std::atoi(e)));
  for (int i = 0; i < want; ++i) {
    // a copy of the shadow statics, at the same alignment modulo 128 as the original
    ctx.r3.u64 = size + 256;
    ctx.r4.u64 = 16;
    sub_8234FB98(ctx, base);
    const uint32_t raw = ctx.r3.u32;
    if (!raw) break;
    const uint32_t copy = raw + ((kShadowLo - raw) & 0x7F);
    std::memcpy(P(copy), P(kShadowLo), size);
    g_fe_delta[i] = int32_t(copy - kShadowLo);
    ctx.r3.u64 = 0;
    ctx.r4.u64 = 0x100000;     // 1 MB guest stack (shadow volume code uses large stack arrays)
    ctx.r5.u64 = 0x822D2610;   // thread procedure override (marker below)
    ctx.r6.u64 = kHelperMagic | uint32_t(i);
    ctx.r7.u64 = 0;
    ctx.r8.u64 = 0;
    sub_82854B18(ctx, base);
  }
  ctx = saved;
  std::unique_lock<std::mutex> lk(g_fe_mu2);
  g_fe_cv.wait_for(lk, std::chrono::seconds(5), [&] { return g_fe_helpers >= want; });
  q4hle::Log("fe jobs: %d helper threads for interaction creation", g_fe_helpers);
}
}  // namespace

// R_AddLightSurfaces' interaction loop (0x8228DCC0): create pending interactions in parallel first
REX_EXTERN(__imp__sub_8228DCC0);
extern "C" REX_FUNC(sub_8228DCC0) {
  if (!FeEnabled() || !g_worker_running.load(std::memory_order_relaxed) ||
      ThreadId() != g_game_tid.load(std::memory_order_relaxed))
    return __imp__sub_8228DCC0(ctx, base);
  if (!g_fe_started) StartFeHelpers(ctx, base);
  if (g_fe_helpers == 0) return __imp__sub_8228DCC0(ctx, base);

  const PPCContext saved = ctx;
  const uint32_t view = Ld(kViewDef);
  const uint32_t viewCount = Ld(kViewCount);
  g_fe_groups.clear();
  g_fe_inters.clear();
  std::unordered_map<uint32_t, uint32_t> key_job;
  std::vector<uint32_t> cand;
  for (uint32_t vEnt = Ld(view + 0x1D8); vEnt; vEnt = Ld(vEnt)) {
    const uint32_t ent = Ld(vEnt + 4);
    if (!ent) continue;
    cand.clear();
    for (uint32_t inter = Ld(ent + 0x1A0); inter && Ld(inter) != 0; inter = Ld(inter + 0x18)) {
      const uint32_t light = Ld(inter + 0xC);
      if (light && Ld(light + 0x498) == viewCount) cand.push_back(inter);
    }
    if (cand.empty()) continue;
    // AddActiveInteraction: dynamic model with surfaces
    ctx.r3.u64 = ent;
    ctx.r4.u64 = 0;
    sub_8228CA18(ctx, base);
    const uint32_t model = ctx.r3.u32;
    if (!model || int32_t(CallVirtual(ctx, base, model, 0x54)) < 1) continue;
    const uint32_t first = uint32_t(g_fe_inters.size());
    for (uint32_t inter : cand) {
      // light-volume culling (only checked by the original when the model is not a static one)
      ctx.r3.u64 = inter;
      sub_82212028(ctx, base);
      if (ctx.r3.u32 & 0xFF) {
        const uint32_t hModel = Ld(ent + 0x28);
        if (hModel && (CallVirtual(ctx, base, hModel, 0x6C) & 0xFF) == 0) {
          ctx.r3.u64 = inter;
          ctx.r4.u64 = view + 0x230;
          sub_82212558(ctx, base);
          if (ctx.r3.u32 & 0xFF) continue;
        }
      }
      // surfaces of an older model: freed and recreated, as AddActiveInteraction does
      if (Ld(inter) != 0xFFFFFFFFu && Ld(ent + 0x160) != Ld(inter + 0x6C)) {
        ctx.r3.u64 = inter;
        sub_82211D38(ctx, base);
      }
      St(inter + 0x6C, Ld(ent + 0x160));
      if (Ld(inter) == 0xFFFFFFFFu) g_fe_inters.push_back(inter);
    }
    const uint32_t n = uint32_t(g_fe_inters.size()) - first;
    if (n) {
      // one job per entity (the MD5R mesh data entities of one model share is mapped under
      // FeLock, see R_DeriveFacePlanes 0x822A4C98 below)
      const uint32_t key = ent;
      auto it = key_job.find(key);
      if (it == key_job.end()) {
        key_job.emplace(key, uint32_t(g_fe_groups.size()));
        g_fe_groups.push_back({});
        it = key_job.find(key);
      }
      g_fe_groups[it->second].push_back(FeJob{model, first, n});
    }
  }
  ctx = saved;
  if (g_fe_groups.size() >= 2) {
    // largest groups first for balance
    std::sort(g_fe_groups.begin(), g_fe_groups.end(), [](const std::vector<FeJob>& a, const std::vector<FeJob>& b) {
      uint32_t na = 0, nb = 0;
      for (auto& j : a) na += j.count;
      for (auto& j : b) nb += j.count;
      return na > nb;
    });
    g_fe_next = 0;
    g_fe_done_helpers = 0;
    g_fe_phase.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lk(g_fe_mu2);
      ++g_fe_gen;
    }
    g_fe_cv.notify_all();
    RunFeJobs(ctx, base);
    {
      std::unique_lock<std::mutex> lk(g_fe_mu2);
      g_fe_cv.wait(lk, [] { return g_fe_done_helpers.load() >= uint32_t(g_fe_helpers); });
    }
    g_fe_phase.store(false, std::memory_order_release);
    ctx = saved;
    ++g_fe_batches;
    g_fe_created += g_fe_inters.size();
    if ((g_fe_batches & 4095) == 0)
      q4hle::Log("fe jobs: %llu batches, %llu interactions created in parallel",
                 (unsigned long long)g_fe_batches, (unsigned long long)g_fe_created);
  } else if (!g_fe_inters.empty()) {
    for (const FeJob& job : g_fe_groups[0])
      for (uint32_t k = 0; k < job.count; ++k) {
        ctx.r3.u64 = g_fe_inters[job.first + k];
        ctx.r4.u64 = job.model;
        __imp__sub_822128F8(ctx, base);
      }
    ctx = saved;
  }
  __imp__sub_8228DCC0(ctx, base);
}

// serialised while helpers run
#define Q4_FE_LOCKED(addr)                                  \
  REX_EXTERN(__imp__sub_##addr);                            \
  extern "C" REX_FUNC(sub_##addr) {                         \
    FeLock fl;                                              \
    __imp__sub_##addr(ctx, base);                           \
  }
Q4_FE_LOCKED(82291530)  // R_FrameAlloc
Q4_FE_LOCKED(822A69A0)  // triangle surface header alloc
Q4_FE_LOCKED(822A6928)  // triangle surface header pool
Q4_FE_LOCKED(822A4168)  // R_FreeStaticTriSurf (deferred-free list)
Q4_FE_LOCKED(82211F88)  // idInteraction::MakeEmpty (relinks into the light's interaction list)
Q4_FE_LOCKED(822A4C98)  // R_DeriveFacePlanes: lazily fills shared surfaces; for MD5R maps the model's shared index buffer
#undef Q4_FE_LOCKED
