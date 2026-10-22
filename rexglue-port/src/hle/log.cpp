#include "hle/log.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace q4hle {
namespace {
std::mutex g_mu;
FILE* g_f = nullptr;
auto g_t0 = std::chrono::steady_clock::now();
}  // namespace

void Log(const char* fmt, ...) {
  std::lock_guard<std::mutex> lock(g_mu);
  if (!g_f) {
    std::string dir = ".";
    if (const char* d = std::getenv("Q4_TRACE_DIR")) dir = d;
    g_f = std::fopen((dir + "/q4hle.log").c_str(), "w");
    if (!g_f) return;
  }
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
  std::fprintf(g_f, "[%8.3f] ", t);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(g_f, fmt, ap);
  va_end(ap);
  std::fputc('\n', g_f);
  std::fflush(g_f);
}
}  // namespace q4hle
