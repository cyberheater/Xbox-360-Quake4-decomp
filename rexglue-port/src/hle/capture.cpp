#include "hle/capture.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include "hle/guest.h"
#include "hle/log.h"
#include "hle/trace.h"

namespace q4hle::capture {
namespace {

std::mutex g_mu;
FILE* g_f = nullptr;
uint64_t g_target = ~0ull;
bool g_done = false;
std::unordered_set<uint64_t> g_blobs;

double g_target_time = -1.0;
std::chrono::steady_clock::time_point g_t0 = std::chrono::steady_clock::now();

struct Init {
  Init() {
    if (const char* e = std::getenv("Q4_HLE_CAPTURE_FRAME")) g_target = std::strtoull(e, nullptr, 10);
    if (const char* e = std::getenv("Q4_HLE_CAPTURE_TIME")) g_target_time = std::atof(e);
  }
} g_init;

void Rec(uint32_t type, const void* data, uint32_t size) {
  std::fwrite(&type, 4, 1, g_f);
  std::fwrite(&size, 4, 1, g_f);
  std::fwrite(data, 1, size, g_f);
}

void Blob(uint8_t* base, uint32_t addr, uint32_t size) {
  if (!addr || !size || size > (1u << 20)) return;
  const uint8_t* p = guest::Ptr(base, addr);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ull;
  h ^= (uint64_t(addr) << 32) ^ size;
  if (!g_blobs.insert(h).second) return;
  std::string buf(8 + size, '\0');
  std::memcpy(&buf[0], &addr, 4);
  std::memcpy(&buf[4], &size, 4);
  std::memcpy(&buf[8], p, size);
  Rec(2, buf.data(), static_cast<uint32_t>(buf.size()));
}

}  // namespace

bool Active() {
  if (g_done) return false;
  if (g_target_time >= 0.0 && g_target == ~0ull &&
      std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count() >= g_target_time)
    g_target = trace::FrameCount() + 1;  // the next whole frame
  return trace::FrameCount() == g_target;
}

void Call(uint32_t fn, const PPCContext& ctx, uint8_t* base, bool is_draw) {
  if (!Active()) return;
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_f) {
    std::string dir = ".";
    if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
    g_f = std::fopen((dir + "/capture.bin").c_str(), "wb");
    if (!g_f) {
      g_done = true;
      return;
    }
    Log("capture: frame %llu", static_cast<unsigned long long>(g_target));
  }
  uint32_t args[10] = {fn,           ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                       ctx.r7.u32,   ctx.r8.u32, ctx.r9.u32, ctx.r10.u32, static_cast<uint32_t>(ctx.lr)};
  if (!is_draw) {
    Rec(3, args, sizeof(args));
    if (fn == 0x825ED210 && ctx.r6.u32) Blob(base, ctx.r6.u32, 0x40);  // resolve destination texture
    if (fn == 0x825EC990) {  // clear: Z is in f1
      const float z = float(ctx.f1.f64);
      uint32_t zb;
      std::memcpy(&zb, &z, 4);
      Rec(4, &zb, 4);
    }
    return;
  }
  const uint32_t dev = ctx.r3.u32;
  std::string buf(sizeof(args) + guest::kDeviceDumpSize, '\0');
  std::memcpy(&buf[0], args, sizeof(args));
  std::memcpy(&buf[sizeof(args)], guest::Ptr(base, dev), guest::kDeviceDumpSize);
  Rec(1, buf.data(), static_cast<uint32_t>(buf.size()));
  // shader microcode as uploaded for this draw
  const uint32_t vs = guest::Load32(base, dev + guest::dev::kVertexShaderBound);
  const uint32_t ps = guest::Load32(base, dev + guest::dev::kPixelShaderBound);
  if (vs) {
    Blob(base, vs, 0x300);  // the shader object itself
    Blob(base, guest::Load32(base, vs + guest::obj::kVsUcodeAddr), guest::Load32(base, vs + guest::obj::kVsUcodeSize));
  }
  if (ps) {
    Blob(base, ps, 0x60);
    Blob(base, guest::Load32(base, ps + guest::obj::kPsUcodeAddr) & ~1u, guest::Load32(base, ps + guest::obj::kPsUcodeSize));
  }
}

void FrameEnd(uint64_t frame) {
  if (g_done || !g_f || frame < g_target) return;
  std::lock_guard<std::mutex> lock(g_mu);
  std::fclose(g_f);
  g_f = nullptr;
  g_done = true;
  Log("capture: written");
}

}  // namespace q4hle::capture
