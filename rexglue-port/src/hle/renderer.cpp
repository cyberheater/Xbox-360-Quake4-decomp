// q4hle - native renderer: replays Quake 4's D3D calls with Direct3D 11.
//
// Each hooked draw first runs the guest D3D function (which updates the guest device's register
// shadows and dirty state), then reads the complete GPU state from the device shadow (see
// guest.h) and issues the equivalent D3D11 draw:
//   - shaders: the patched microcode the guest uploaded, translated by xenos_hlsl.cpp
//   - constants: float/bool/loop constants from the device shadow
//   - vertices: vertex fetch constants -> raw buffers read by the translated vertex shader
//   - indices: the guest index buffer object, endian-swapped
//   - textures/samplers: texture fetch constants (textures.cpp)
//   - render targets: EDRAM surfaces (RB_SURFACE_INFO / RB_COLOR_INFO / RB_DEPTH_INFO) mapped to
//     native textures; Resolve copies them to the texture the guest resolves into; Swap presents
//     the front buffer texture.

#include "hle/renderer.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <utility>
#include <mutex>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "hle/capture.h"
#include "hle/guest.h"
#include "hle/log.h"
#include "hle/native.h"
#include "hle/textures.h"
#include "hle/trace.h"
#include "hle/xenos_hlsl.h"

#include <rex/cvar.h>


#pragma comment(lib, "d3dcompiler.lib")

namespace q4hle {
void InstallCrashLogger();
}

namespace q4hle::render {
namespace {

using trace::GuestFn;
using guest::Load32;

// ---------------------------------------------------------------------------------------------
// Registers
struct Regs {
  const uint8_t* base;
  uint32_t dev;
  uint32_t operator()(uint32_t reg) const {
    const uint32_t off = guest::dev::RegOffset(reg);
    return off ? Load32(base, dev + off) : 0;
  }
  float F(uint32_t reg) const {
    const uint32_t u = (*this)(reg);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
};

enum : uint32_t {
  RB_SURFACE_INFO = 0x2000, RB_COLOR_INFO = 0x2001, RB_DEPTH_INFO = 0x2002, RB_COLOR1_INFO = 0x2003,
  PA_SC_SCREEN_SCISSOR_TL = 0x200E, PA_SC_SCREEN_SCISSOR_BR = 0x200F,
  VGT_INDX_OFFSET = 0x2102, RB_COLOR_MASK = 0x2104, RB_BLEND_RED = 0x2105,
  RB_STENCILREFMASK_BF = 0x210C, RB_STENCILREFMASK = 0x210D, RB_ALPHA_REF = 0x210E,
  PA_CL_VPORT_XSCALE = 0x210F, PA_CL_VPORT_XOFFSET = 0x2110, PA_CL_VPORT_YSCALE = 0x2111,
  PA_CL_VPORT_YOFFSET = 0x2112, PA_CL_VPORT_ZSCALE = 0x2113, PA_CL_VPORT_ZOFFSET = 0x2114,
  SQ_PROGRAM_CNTL = 0x2180, SQ_CONTEXT_MISC = 0x2181,
  RB_DEPTHCONTROL = 0x2200, RB_BLENDCONTROL0 = 0x2201, RB_COLORCONTROL = 0x2202, PA_CL_CLIP_CNTL = 0x2204,
  PA_SU_SC_MODE_CNTL = 0x2205, PA_CL_VTE_CNTL = 0x2206, RB_MODECONTROL = 0x2208, RB_BLENDCONTROL1 = 0x2209,
  PA_SU_VTX_CNTL = 0x2302,
  PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380, PA_SU_POLY_OFFSET_FRONT_OFFSET = 0x2381,
};

// ---------------------------------------------------------------------------------------------
// Helpers
template <typename T>
void SafeRelease(T*& p) {
  if (p) p->Release();
  p = nullptr;
}

uint64_t Fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

std::string g_dir;
bool g_dump_shaders = false;
uint64_t g_frame = 0;      // frames executed (render thread)
uint64_t g_cap_frame = 0;  // frames captured (game thread)
void DeferRelease(IUnknown* p);

const std::string& OutDir() {
  if (g_dir.empty()) {
    const char* d = std::getenv("Q4_TRACE_DIR");
    g_dir = d ? d : ".";
    const char* ds = std::getenv("Q4_HLE_DUMP_SHADERS");
    g_dump_shaders = ds && *ds == '1';
  }
  return g_dir;
}

// ---------------------------------------------------------------------------------------------
// Shaders
// Per-section CPU time spent on the game's render thread (logged every 300 frames).
enum ProfSection { kProfDraw, kProfShader, kProfIndex, kProfVb, kProfConst, kProfTex, kProfSubmit, kProfClearResolve, kProfFrame, kProfCount };
const char* const kProfNames[kProfCount] = {"draw", "shader", "index", "vb", "const", "tex", "submit", "clear/resolve", "endframe"};
uint64_t g_prof[kProfCount] = {};
uint64_t g_prof_bytes_hashed = 0, g_prof_bytes_uploaded = 0, g_prof_cb_uploads = 0;
uint64_t g_prof_vb_midframe = 0;  // vertex stripes rewritten mid-frame (caught by the hot-stripe check)
inline uint64_t Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return uint64_t(t.QuadPart);
}
struct ProfScope {
  int s;
  uint64_t t0 = Now();
  explicit ProfScope(int sec) : s(sec) {}
  ~ProfScope() { g_prof[s] += Now() - t0; }
};

struct ShaderEntry {
  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  xenos::TranslatedShader info;
  bool failed = false;
};
std::unordered_map<uint64_t, ShaderEntry> g_shaders;
ID3D11GeometryShader* g_rect_gs = nullptr;

std::filesystem::path CacheDir() {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  return std::filesystem::path(exe).parent_path() / "q4hle_shader_cache";
}

ID3DBlob* Compile(const std::string& src, const char* profile, uint64_t key, std::string& errors) {
  // disk cache of compiled bytecode keyed by the HLSL
  const uint64_t hk = Fnv(src.data(), src.size(), key);
  std::error_code ec;
  const auto dir = CacheDir();
  std::filesystem::create_directories(dir, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx_%s.cso", static_cast<unsigned long long>(hk), profile);
  const auto path = dir / name;
  if (FILE* f = _wfopen(path.c_str(), L"rb")) {
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    ID3DBlob* blob = nullptr;
    if (n > 0 && SUCCEEDED(D3DCreateBlob(size_t(n), &blob)) && std::fread(blob->GetBufferPointer(), 1, size_t(n), f) == size_t(n)) {
      std::fclose(f);
      return blob;
    }
    SafeRelease(blob);
    std::fclose(f);
  }
  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT hr = D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main", profile,
                          D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err);
  if (err) {
    errors.assign(static_cast<const char*>(err->GetBufferPointer()), err->GetBufferSize());
    err->Release();
  }
  if (FAILED(hr)) {
    SafeRelease(blob);
    return nullptr;
  }
  if (FILE* f = _wfopen(path.c_str(), L"wb")) {
    std::fwrite(blob->GetBufferPointer(), 1, blob->GetBufferSize(), f);
    std::fclose(f);
  }
  return blob;
}

void DumpShader(uint64_t key, const ShaderEntry& e, const std::string& errors) {
  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::path(OutDir()) / "shaders";
  std::filesystem::create_directories(dir, ec);
  char name[64];
  std::snprintf(name, sizeof(name), "%016llx_%s", static_cast<unsigned long long>(key), e.info.pixel ? "ps" : "vs");
  if (FILE* f = std::fopen((dir / (std::string(name) + ".hlsl")).string().c_str(), "w")) {
    std::fputs(e.info.hlsl.c_str(), f);
    if (!errors.empty() || !e.info.error.empty()) std::fprintf(f, "\n/* errors:\n%s\n%s\n*/\n", e.info.error.c_str(), errors.c_str());
    std::fclose(f);
  }
}

struct ShaderBinding {
  ShaderEntry* entry = nullptr;
  std::vector<uint8_t> ucode;  // microcode bytes the entry was looked up with
};
std::unordered_map<uint64_t, ShaderBinding> g_shader_bindings;

ShaderEntry* GetShaderSlow(const uint8_t* base, uint32_t ucode_addr, uint32_t bytes, bool pixel,
                           const xenos::TranslateOptions& opt);

// The microcode at an address can change between draws of the same frame: D3D patches vertex
// shaders for the bound vertex declaration in place. Each draw compares the bytes with the copy
// the binding was made from (a memcmp of a few hundred bytes) and re-resolves when they differ.
ShaderEntry* GetShader(const uint8_t* base, uint32_t ucode_addr, uint32_t bytes, bool pixel,
                       const xenos::TranslateOptions& opt) {
  if (!ucode_addr || !bytes || bytes > 0x10000) return nullptr;
  const uint64_t bkey = (uint64_t(ucode_addr) << 32) ^ (uint64_t(bytes) << 1) ^ (pixel ? 1 : 0) ^
                        (uint64_t(opt.param_gen_reg + 1) << 20) ^ (uint64_t(opt.interpolator_count) << 26);
  ShaderBinding& b = g_shader_bindings[bkey];
  const uint8_t* p = guest::Ptr(base, ucode_addr);
  if (b.ucode.size() == bytes && std::memcmp(b.ucode.data(), p, bytes) == 0) return b.entry;
  b.entry = GetShaderSlow(base, ucode_addr, bytes, pixel, opt);
  b.ucode.assign(p, p + bytes);
  return b.entry;
}

ShaderEntry* GetShaderSlow(const uint8_t* base, uint32_t ucode_addr, uint32_t bytes, bool pixel,
                           const xenos::TranslateOptions& opt) {
  if (!ucode_addr || !bytes || bytes > 0x10000) return nullptr;
  const uint8_t* p = guest::Ptr(base, ucode_addr);
  uint64_t key = Fnv(p, bytes, pixel ? 0x9E3779B97F4A7C15ull : 0x5bd1e995ull);
  key ^= uint64_t(opt.param_gen_reg + 1) * 0x100000001B3ull ^ (uint64_t(opt.interpolator_count) << 56);
  auto it = g_shaders.find(key);
  if (it != g_shaders.end()) return it->second.failed ? nullptr : &it->second;
  ShaderEntry& e = g_shaders[key];
  std::vector<uint32_t> words(bytes / 4);
  for (size_t i = 0; i < words.size(); ++i) words[i] = Load32(base, ucode_addr + uint32_t(i * 4));
  e.info = xenos::Translate(words.data(), words.size(), pixel, opt);
  std::string errors;
  ID3DBlob* blob = Compile(e.info.hlsl, pixel ? "ps_5_0" : "vs_5_0", key, errors);
  if (!blob) {
    e.failed = true;
    Log("shader %016llx (%s) failed to compile: %s %s", static_cast<unsigned long long>(key), pixel ? "ps" : "vs",
        e.info.error.c_str(), errors.substr(0, 300).c_str());
    DumpShader(key, e, errors);
    return nullptr;
  }
  HRESULT hr = pixel ? native::Device()->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.ps)
                     : native::Device()->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &e.vs);
  blob->Release();
  if (FAILED(hr)) e.failed = true;
  if (!e.info.error.empty()) Log("shader %016llx (%s): %s", static_cast<unsigned long long>(key), pixel ? "ps" : "vs", e.info.error.c_str());
  if (g_dump_shaders || !e.info.error.empty()) DumpShader(key, e, errors);
  return e.failed ? nullptr : &e;
}

// Rectangle lists: each triangle (v0, v1, v2) is a rectangle with the 4th vertex v1 + v2 - v0.
const char kRectGs[] = R"(
struct V { float4 pos : SV_Position; float4 v[16] : TEXCOORD0; float2 sprite : XESPRITE; float psize : XEPSIZE; };
V Lerp(V a, V b, V c) { V o; o.pos = b.pos + c.pos - a.pos; [unroll] for (int i = 0; i < 16; ++i) o.v[i] = b.v[i] + c.v[i] - a.v[i]; o.sprite = a.sprite; o.psize = a.psize; return o; }
[maxvertexcount(4)]
void main(triangle V t[3], inout TriangleStream<V> s) {
  // Find the vertex at the right angle (the one opposite the longest edge) to complete the rectangle.
  float2 a = t[0].pos.xy / t[0].pos.w, b = t[1].pos.xy / t[1].pos.w, c = t[2].pos.xy / t[2].pos.w;
  float d01 = dot(a - b, a - b), d12 = dot(b - c, b - c), d20 = dot(c - a, c - a);
  if (d12 >= d01 && d12 >= d20) { s.Append(t[1]); s.Append(t[0]); s.Append(t[2]); s.Append(Lerp(t[0], t[1], t[2])); }
  else if (d20 >= d01) { s.Append(t[0]); s.Append(t[1]); s.Append(t[2]); s.Append(Lerp(t[1], t[0], t[2])); }
  else { s.Append(t[0]); s.Append(t[2]); s.Append(t[1]); s.Append(Lerp(t[2], t[0], t[1])); }
}
)";

// Point lists: each point becomes a screen-aligned square (Xenos point sprites, after Xenia).
const char kPointGs[] = R"(
cbuffer XeDraw : register(b1) {
  uint4 xe_bool[2]; uint4 xe_loop[32]; float4 xe_ndc_scale; float4 xe_ndc_offset; float4 xe_alpha;
  uint4 xe_tex[32]; float4 xe_misc; float4 xe_point; float4 xe_point2;
};
struct V { float4 pos : SV_Position; float4 v[16] : TEXCOORD0; float2 sprite : XESPRITE; float psize : XEPSIZE; };
[maxvertexcount(4)]
void main(point V p[1], inout TriangleStream<V> s) {
  float2 d = p[0].psize > 0.0 ? clamp(p[0].psize, xe_point.z, xe_point.w).xx : xe_point.xy;
  float2 r = d * xe_point2.xy * p[0].pos.w;
  V o = p[0];
  o.pos.xy = p[0].pos.xy + float2(-r.x, r.y); o.sprite = float2(0, 0); s.Append(o);
  o.pos.xy = p[0].pos.xy + float2(r.x, r.y);  o.sprite = float2(1, 0); s.Append(o);
  o.pos.xy = p[0].pos.xy + float2(-r.x, -r.y); o.sprite = float2(0, 1); s.Append(o);
  o.pos.xy = p[0].pos.xy + float2(r.x, -r.y);  o.sprite = float2(1, 1); s.Append(o);
}
)";
ID3D11GeometryShader* g_point_gs = nullptr;

// ---------------------------------------------------------------------------------------------
// Dynamic buffers (ring allocated per frame)
struct Ring {
  ID3D11Buffer* buf = nullptr;
  uint32_t size = 0, pos = 0;
  UINT bind = 0;
  bool raw = false;
  void Create(uint32_t bytes, UINT bind_flags, bool raw_views) {
    size = bytes;
    bind = bind_flags;
    raw = raw_views;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = bind_flags;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = raw_views ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS : 0;
    native::Device()->CreateBuffer(&bd, nullptr, &buf);
  }
  // Returns the write pointer and the offset; the caller unmaps.
  uint8_t* Alloc(uint32_t bytes, uint32_t align, uint32_t& offset) {
    pos = (pos + align - 1) & ~(align - 1);
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (pos + bytes > size) {
      pos = 0;
      mode = D3D11_MAP_WRITE_DISCARD;
    }
    if (bytes > size) return nullptr;
    D3D11_MAPPED_SUBRESOURCE ms;
    if (FAILED(native::Context()->Map(buf, 0, mode, 0, &ms))) return nullptr;
    offset = pos;
    pos += bytes;
    return static_cast<uint8_t*>(ms.pData) + offset;
  }
  void Unmap() { native::Context()->Unmap(buf, 0); }
};

Ring g_vb_ring, g_ib_ring;
ID3D11Buffer* g_cb_vs = nullptr;
ID3D11Buffer* g_cb_ps = nullptr;
ID3D11Buffer* g_cb_draw = nullptr;

struct XeDrawCB {
  uint32_t bools[8];
  uint32_t loops[32][4];
  float ndc_scale[4];
  float ndc_offset[4];
  float alpha[4];
  uint32_t tex[32][4];
  float misc[4];
  float point[4];
  float point2[4];
};

void UpdateCB(ID3D11Buffer* b, const void* data, size_t n) {
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(native::Context()->Map(b, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
    std::memcpy(ms.pData, data, n);
    native::Context()->Unmap(b, 0);
  }
}

ID3D11Buffer* MakeCB(uint32_t bytes) {
  D3D11_BUFFER_DESC bd{};
  bd.ByteWidth = (bytes + 15) & ~15u;
  bd.Usage = D3D11_USAGE_DYNAMIC;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  ID3D11Buffer* b = nullptr;
  native::Device()->CreateBuffer(&bd, nullptr, &b);
  return b;
}

// ---------------------------------------------------------------------------------------------
// Render targets (EDRAM surfaces)
struct RenderTarget {
  ID3D11Texture2D* tex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11DepthStencilView* dsv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint32_t w = 0, h = 0;
  DXGI_FORMAT copy_format = DXGI_FORMAT_UNKNOWN;  // typeless family used for resolves
  DXGI_FORMAT view_format = DXGI_FORMAT_UNKNOWN;
  bool depth = false;
};
std::unordered_map<uint64_t, RenderTarget> g_rts;

uint32_t g_rt_height = 1024;
// Internal resolution scale: render targets and resolves are g_scale x the guest size.
float g_scale = 1.0f;
// a guest length in render-target pixels
inline uint32_t Sc(uint32_t v) { return uint32_t(std::lround(double(v) * g_scale)); }
inline int32_t Sc(int32_t v) { return int32_t(std::lround(double(v) * g_scale)); }

// Stereoscopic 3D state (see "Stereoscopic 3D" below).
int g_stereo_mode = 0, g_stereo_depth = 2;
bool g_st_pending = false, g_st_have = false;  // this frame: 3D drawn since the last copy / copy made
RenderTarget* g_st_rt = nullptr;
RenderTarget* g_st_depth_rt = nullptr;
float g_depth_clear = 1.0f;  // last depth clear value (1: near is small depth; 0: reversed)
void RefreshStereoSettings();

// CRT filter state (see "CRT filter" below).
int g_crt_preset = 0;  // q4hle_crt_filter: 0 off, 1-3 grille 240/360/480, 4-6 slot mask
void RefreshCrtSettings();

// Q4_HLE_SCALE (1-4) if set, else the resolution_scale cvar (GRAPHICS menu). Changes of the cvar
// apply between frames (ApplyScaleChange).
// (The SDK's cvars are read by name: their storage is not exported from the runtime DLL.)
float DesiredScale() {
  if (const char* sc = std::getenv("Q4_HLE_SCALE"); sc && *sc) return float(std::clamp(std::atof(sc), 1.0, 4.0));
  // q4hle_render_height (GRAPHICS menu: 720 ... 2880 lines) wins over the whole-number resolution_scale
  const std::string hgt = rex::cvar::GetFlagByName("q4hle_render_height");
  if (const int hh = hgt.empty() ? 0 : std::atoi(hgt.c_str()); hh >= 720) return std::min(4.0f, float(hh) / 720.0f);
  const std::string v = rex::cvar::GetFlagByName("resolution_scale");
  return float(std::clamp(v.empty() ? 1 : std::atoi(v.c_str()), 1, 4));
}

RenderTarget* GetRT(uint32_t edram_base, uint32_t format, uint32_t pitch, bool depth) {
  if (!pitch) return nullptr;
  const uint64_t key = uint64_t(edram_base) | (uint64_t(format) << 12) | (uint64_t(pitch) << 16) | (uint64_t(depth) << 40);
  RenderTarget& rt = g_rts[key];
  if (rt.tex) return &rt;
  rt.w = Sc(pitch);
  rt.h = Sc(uint32_t(g_rt_height));
  rt.depth = depth;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = rt.w;
  td.Height = rt.h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  if (depth) {
    const bool fl = format == 1;
    td.Format = fl ? DXGI_FORMAT_R32G8X24_TYPELESS : DXGI_FORMAT_R24G8_TYPELESS;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    native::Device()->CreateTexture2D(&td, nullptr, &rt.tex);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = fl ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D24_UNORM_S8_UINT;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    native::Device()->CreateDepthStencilView(rt.tex, &dd, &rt.dsv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = fl ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    native::Device()->CreateShaderResourceView(rt.tex, &sd, &rt.srv);
    rt.copy_format = td.Format;
    rt.view_format = sd.Format;
  } else {
    DXGI_FORMAT tf = DXGI_FORMAT_R8G8B8A8_TYPELESS, vf = DXGI_FORMAT_R8G8B8A8_UNORM;
    switch (format) {
      case 0: break;
      case 1: vf = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; break;
      case 2: case 10: tf = DXGI_FORMAT_R10G10B10A2_TYPELESS; vf = DXGI_FORMAT_R10G10B10A2_UNORM; break;
      case 3: case 12: case 7: tf = DXGI_FORMAT_R16G16B16A16_TYPELESS; vf = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
      case 4: tf = DXGI_FORMAT_R16G16_TYPELESS; vf = DXGI_FORMAT_R16G16_SNORM; break;
      case 5: tf = DXGI_FORMAT_R16G16B16A16_TYPELESS; vf = DXGI_FORMAT_R16G16B16A16_SNORM; break;
      case 6: tf = DXGI_FORMAT_R16G16_TYPELESS; vf = DXGI_FORMAT_R16G16_FLOAT; break;
      case 14: tf = DXGI_FORMAT_R32_TYPELESS; vf = DXGI_FORMAT_R32_FLOAT; break;
      case 15: tf = DXGI_FORMAT_R32G32_TYPELESS; vf = DXGI_FORMAT_R32G32_FLOAT; break;
      default: break;
    }
    td.Format = tf;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    native::Device()->CreateTexture2D(&td, nullptr, &rt.tex);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = vf;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    native::Device()->CreateRenderTargetView(rt.tex, &rd, &rt.rtv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = vf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM : vf;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    native::Device()->CreateShaderResourceView(rt.tex, &sd, &rt.srv);
    rt.copy_format = tf;
    rt.view_format = sd.Format;
    const float black[4] = {0, 0, 0, 0};
    native::Context()->ClearRenderTargetView(rt.rtv, black);
  }
  Log("rt: created %s base %u format %u %ux%u", depth ? "depth" : "color", edram_base, format, rt.w, rt.h);
  return &rt;
}

struct BoundTargets {
  RenderTarget* color[4] = {};
  RenderTarget* depth = nullptr;
  uint32_t pitch = 0;
};

BoundTargets CurrentTargets(const Regs& R) {
  BoundTargets b;
  const uint32_t si = R(RB_SURFACE_INFO);
  b.pitch = si & 0x3FFF;
  const uint32_t ci[4] = {R(RB_COLOR_INFO), R(RB_COLOR1_INFO), R(RB_COLOR1_INFO + 1), R(RB_COLOR1_INFO + 2)};
  const uint32_t mask = R(RB_COLOR_MASK);
  const uint32_t mode = R(RB_MODECONTROL) & 7;  // 4 = color+depth, 5 = depth only (EdramMode)
  for (int i = 0; i < 4; ++i) {
    if (i > 0 && !((mask >> (4 * i)) & 0xF)) continue;
    if (mode == 5) continue;
    b.color[i] = GetRT(ci[i] & 0xFFF, (ci[i] >> 16) & 0xF, b.pitch, false);
  }
  const uint32_t di = R(RB_DEPTH_INFO);
  b.depth = GetRT(di & 0xFFF, (di >> 16) & 1, b.pitch, true);
  return b;
}

// ---------------------------------------------------------------------------------------------
// Pipeline state objects
std::unordered_map<uint64_t, ID3D11BlendState*> g_blend;
std::unordered_map<uint64_t, ID3D11DepthStencilState*> g_ds;
std::unordered_map<uint64_t, ID3D11RasterizerState*> g_rs;

D3D11_BLEND Blend(uint32_t f, bool alpha) {
  switch (f) {
    case 0: return D3D11_BLEND_ZERO;
    case 1: return D3D11_BLEND_ONE;
    case 4: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 5: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 6: return D3D11_BLEND_SRC_ALPHA;
    case 7: return D3D11_BLEND_INV_SRC_ALPHA;
    case 8: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 9: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 10: return D3D11_BLEND_DEST_ALPHA;
    case 11: return D3D11_BLEND_INV_DEST_ALPHA;
    case 12: case 14: return D3D11_BLEND_BLEND_FACTOR;
    case 13: case 15: return D3D11_BLEND_INV_BLEND_FACTOR;
    case 16: return alpha ? D3D11_BLEND_ONE : D3D11_BLEND_SRC_ALPHA_SAT;
    default: return D3D11_BLEND_ONE;
  }
}
D3D11_BLEND_OP BlendOp(uint32_t op) {
  switch (op) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
  }
}

ID3D11BlendState* GetBlend(const Regs& R) {
  const uint32_t bc[4] = {R(RB_BLENDCONTROL0), R(RB_BLENDCONTROL1), R(RB_BLENDCONTROL1 + 1), R(RB_BLENDCONTROL1 + 2)};
  const uint32_t mask = R(RB_COLOR_MASK) & 0xFFFF;
  uint64_t key = Fnv(bc, sizeof(bc), mask);
  auto it = g_blend.find(key);
  if (it != g_blend.end()) return it->second;
  D3D11_BLEND_DESC d{};
  d.IndependentBlendEnable = TRUE;
  for (int i = 0; i < 4; ++i) {
    auto& t = d.RenderTarget[i];
    const uint32_t b = bc[i];
    const uint32_t cs = b & 31, cop = (b >> 5) & 7, cd = (b >> 8) & 31, as = (b >> 16) & 31, aop = (b >> 21) & 7,
                   ad = (b >> 24) & 31;
    t.BlendEnable = !(cs == 1 && cd == 0 && cop == 0 && as == 1 && ad == 0 && aop == 0);
    t.SrcBlend = Blend(cs, false);
    t.DestBlend = Blend(cd, false);
    t.BlendOp = BlendOp(cop);
    t.SrcBlendAlpha = Blend(as, true);
    t.DestBlendAlpha = Blend(ad, true);
    t.BlendOpAlpha = BlendOp(aop);
    if (cop == 2 || cop == 3) t.SrcBlend = t.DestBlend = D3D11_BLEND_ONE;
    if (aop == 2 || aop == 3) t.SrcBlendAlpha = t.DestBlendAlpha = D3D11_BLEND_ONE;
    t.RenderTargetWriteMask = UINT8((mask >> (4 * i)) & 0xF);
  }
  ID3D11BlendState* s = nullptr;
  native::Device()->CreateBlendState(&d, &s);
  g_blend[key] = s;
  return s;
}

D3D11_COMPARISON_FUNC Cmp(uint32_t f) { return D3D11_COMPARISON_FUNC((f & 7) + 1); }
D3D11_STENCIL_OP SOp(uint32_t o) {
  switch (o & 7) {
    case 0: return D3D11_STENCIL_OP_KEEP;
    case 1: return D3D11_STENCIL_OP_ZERO;
    case 2: return D3D11_STENCIL_OP_REPLACE;
    case 3: return D3D11_STENCIL_OP_INCR_SAT;
    case 4: return D3D11_STENCIL_OP_DECR_SAT;
    case 5: return D3D11_STENCIL_OP_INVERT;
    case 6: return D3D11_STENCIL_OP_INCR;
    default: return D3D11_STENCIL_OP_DECR;
  }
}

// Off by default: relaxing breaks alpha-to-mask holes (lighting passes would fill them).
bool g_relax_equal = [] {
  const char* e = std::getenv("Q4_HLE_RELAX_DEPTH_EQUAL");
  return e && *e == '1';
}();

ID3D11DepthStencilState* GetDepthStencil(const Regs& R, uint32_t& stencil_ref) {
  const uint32_t dc = R(RB_DEPTHCONTROL);
  const uint32_t srm = R(RB_STENCILREFMASK), srm_bf = R(RB_STENCILREFMASK_BF);
  stencil_ref = srm & 0xFF;
  const uint64_t key = uint64_t(dc) | (uint64_t(srm & 0xFFFF00) << 32);
  auto it = g_ds.find(key);
  if (it != g_ds.end()) return it->second;
  (void)srm_bf;
  D3D11_DEPTH_STENCIL_DESC d{};
  d.DepthEnable = (dc >> 1) & 1;
  d.DepthWriteMask = ((dc >> 2) & 1) ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
  d.DepthFunc = Cmp(dc >> 4);
  // Doom 3 style lighting passes test EQUAL against the depth pre-pass. Positions computed by the
  // translated shaders may differ in the last bit, so EQUAL without depth writes becomes LESS_EQUAL.
  if (d.DepthFunc == D3D11_COMPARISON_EQUAL && !((dc >> 2) & 1) && g_relax_equal) d.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
  d.StencilEnable = dc & 1;
  d.StencilReadMask = UINT8((srm >> 8) & 0xFF);
  d.StencilWriteMask = UINT8((srm >> 16) & 0xFF);
  d.FrontFace = {SOp(dc >> 11), SOp(dc >> 17), SOp(dc >> 14), Cmp(dc >> 8)};
  if ((dc >> 7) & 1)
    d.BackFace = {SOp(dc >> 23), SOp(dc >> 29), SOp(dc >> 26), Cmp(dc >> 20)};
  else
    d.BackFace = d.FrontFace;
  if (!d.DepthEnable) {
    d.DepthEnable = d.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;  // Xenos writes depth only when enabled
    d.DepthFunc = D3D11_COMPARISON_ALWAYS;
    if (!((dc >> 1) & 1)) d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO, d.DepthEnable = FALSE;
  }
  ID3D11DepthStencilState* s = nullptr;
  native::Device()->CreateDepthStencilState(&d, &s);
  g_ds[key] = s;
  return s;
}

ID3D11RasterizerState* GetRaster(const Regs& R, bool flip_winding) {
  const uint32_t mc = R(PA_SU_SC_MODE_CNTL), cc = R(PA_CL_CLIP_CNTL);
  const float pscale = R.F(PA_SU_POLY_OFFSET_FRONT_SCALE), poff = R.F(PA_SU_POLY_OFFSET_FRONT_OFFSET);
  const bool offset = (mc >> 11) & 3;
  uint64_t key = uint64_t(mc & 0x1FFF) | (uint64_t((cc >> 16) & 1) << 13) | (uint64_t(flip_winding) << 14);
  if (offset) {
    uint32_t a, b;
    std::memcpy(&a, &pscale, 4);
    std::memcpy(&b, &poff, 4);
    key ^= (uint64_t(a) << 16) ^ (uint64_t(b) << 40) ^ 0x8000;
  }
  auto it = g_rs.find(key);
  if (it != g_rs.end()) return it->second;
  D3D11_RASTERIZER_DESC d{};
  d.FillMode = (((mc >> 3) & 3) == 1 && ((mc >> 5) & 7) == 1) ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
  const bool cf = mc & 1, cb = (mc >> 1) & 1;
  d.CullMode = cf && cb ? D3D11_CULL_NONE : cf ? D3D11_CULL_FRONT : cb ? D3D11_CULL_BACK : D3D11_CULL_NONE;
  bool ccw_front = !((mc >> 2) & 1);
  if (flip_winding) ccw_front = !ccw_front;
  d.FrontCounterClockwise = ccw_front;
  if (offset) {
    // Xenos offsets are in units of the depth buffer's resolution (24-bit).
    d.DepthBias = INT(poff);
    d.SlopeScaledDepthBias = pscale / 16.0f;
  }
  d.DepthClipEnable = !((cc >> 16) & 1);
  d.ScissorEnable = TRUE;
  ID3D11RasterizerState* s = nullptr;
  native::Device()->CreateRasterizerState(&d, &s);
  g_rs[key] = s;
  return s;
}

// Window scissor, from the D3D's internal SetScissor(device, left, top, right, bottom) (0x825E5670),
// which writes PA_SC_WINDOW_SCISSOR_TL/BR directly to the command buffer (not shadowed).
int32_t g_scissor[4] = {0, 0, 8192, 8192};

// ---------------------------------------------------------------------------------------------
// Index conversion
uint32_t ReadIndex(const uint8_t* p, uint32_t i, bool is32) {
  if (is32) {
    uint32_t v;
    std::memcpy(&v, p + 4 * i, 4);
    return __builtin_bswap32(v);
  }
  uint16_t v;
  std::memcpy(&v, p + 2 * i, 2);
  return __builtin_bswap16(v);
}

// Builds a triangle/line list for primitive types D3D11 lacks. Returns the D3D11 topology.
D3D11_PRIMITIVE_TOPOLOGY Topology(uint32_t prim, bool& needs_list, bool& rect) {
  needs_list = false;
  rect = false;
  switch (prim) {
    case 1: return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case 2: return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case 3: return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case 4: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 6: return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case 8: rect = true; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 5: case 13: case 14: case 15: needs_list = true; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case 12: needs_list = true; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    default: return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  }
}

void ExpandIndices(uint32_t prim, const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
  out.clear();
  const size_t n = in.size();
  switch (prim) {
    case 5:  // fan
    case 15: // polygon
      for (size_t i = 2; i < n; ++i) out.insert(out.end(), {in[0], in[i - 1], in[i]});
      break;
    case 13:  // quad list
      for (size_t i = 0; i + 3 < n; i += 4) out.insert(out.end(), {in[i], in[i + 1], in[i + 2], in[i], in[i + 2], in[i + 3]});
      break;
    case 14:  // quad strip
      for (size_t i = 0; i + 3 < n; i += 2) out.insert(out.end(), {in[i], in[i + 1], in[i + 3], in[i], in[i + 3], in[i + 2]});
      break;
    case 12:  // line loop
      out = in;
      if (n) out.push_back(in[0]);
      break;
    default:
      out = in;
  }
}

// ---------------------------------------------------------------------------------------------
// Vertex buffer cache
void SwapCopy(uint32_t* d32, const uint8_t* src, uint32_t words, uint32_t endian) {
  for (uint32_t i = 0; i < words; ++i) {
    uint32_t v;
    std::memcpy(&v, src + size_t(i) * 4, 4);
    switch (endian) {
      case 1: v = ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u); break;
      case 2: v = __builtin_bswap32(v); break;
      case 3: v = (v >> 16) | (v << 16); break;
      default: break;
    }
    d32[i] = v;
  }
}

struct VbEntry {
  ID3D11Buffer* buf = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint32_t bytes = 0;
  std::vector<uint64_t> stripe_hash;   // content hash per stripe
  std::vector<uint64_t> stripe_frame;  // frame in which the stripe was last verified
  std::vector<uint64_t> stripe_changed;  // frame in which the stripe last changed (~0: never)
  // Raw guest bytes of stripes that change often ("hot"). The game appends vertices to such
  // stripes during a frame (GUI, particles, HUD), so every draw compares the bytes it reads.
  std::vector<std::unique_ptr<uint8_t[]>> shadow;
  uint64_t last_used = 0;
};
std::unordered_map<uint64_t, VbEntry> g_vb_cache;
uint64_t g_vb_frame = 0;
constexpr uint32_t kVbStripe = 16u << 10;
constexpr uint64_t kVbHotFrames = 240;  // a stripe that changed within this many frames is hot

uint64_t SampleHashBytes(const uint8_t* p, uint32_t bytes) {
  uint64_t h1 = 0x9E3779B97F4A7C15ull ^ bytes, h2 = 0xC2B2AE3D27D4EB4Full;
  const uint32_t n = bytes / 16;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t a, b;
    std::memcpy(&a, p + size_t(i) * 16, 8);
    std::memcpy(&b, p + size_t(i) * 16 + 8, 8);
    h1 = (h1 ^ a) * 0x100000001B3ull;
    h2 = (h2 ^ b) * 0x100000001B3ull;
    h1 = (h1 << 29) | (h1 >> 35);
  }
  for (uint32_t i = n * 16; i < bytes; ++i) h2 = (h2 ^ p[i]) * 0x100000001B3ull;
  g_prof_bytes_hashed += bytes;
  return h1 ^ (h2 * 0x9E3779B97F4A7C15ull);
}

// Vertex buffers, as declared by their fetch constant, live on the GPU for as long as they are
// used. The buffer is split into 16 KB stripes; each stripe a draw reads is verified once per
// frame (content hash) and re-uploaded if it changed. Static geometry costs one hash per used
// stripe per frame; dynamic data is uploaded only where it was rewritten.
// A stripe range of a cached vertex buffer that changed: uploaded by the render thread before the
// draw that needs it.
// Allocator that leaves new elements uninitialised: capture buffers are resized and then fully
// overwritten, so value-initialising (zero-filling) them first is wasted work on the game thread.
template <typename T>
struct NoInitAlloc : std::allocator<T> {
  template <typename U>
  struct rebind {
    using other = NoInitAlloc<U>;
  };
  NoInitAlloc() = default;
  template <typename U>
  NoInitAlloc(const NoInitAlloc<U>&) {}
  template <typename U>
  void construct(U* p) noexcept {
    ::new (static_cast<void*>(p)) U;
  }
  template <typename U, typename... Args>
  void construct(U* p, Args&&... args) {
    ::new (static_cast<void*>(p)) U(std::forward<Args>(args)...);
  }
};
template <typename T>
using RawVec = std::vector<T, NoInitAlloc<T>>;

struct VbUpload {
  ID3D11Buffer* buf;
  uint32_t off;
  RawVec<uint32_t> data;
};

ID3D11ShaderResourceView* CachedVertexBuffer(const uint8_t* src, uint32_t addr, uint32_t bytes, uint32_t endian,
                                             uint32_t lo, uint32_t hi, std::vector<VbUpload>& uploads) {
  if (bytes < 4096 || bytes > (64u << 20)) return nullptr;
  bytes &= ~3u;
  const uint64_t key = (uint64_t(addr) << 32) ^ (uint64_t(bytes) << 2) ^ endian;
  VbEntry& e = g_vb_cache[key];
  e.last_used = g_vb_frame;
  const uint32_t stripes = (bytes + kVbStripe - 1) / kVbStripe;
  if (!e.buf) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    if (FAILED(native::Device()->CreateBuffer(&bd, nullptr, &e.buf))) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.NumElements = bytes / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    native::Device()->CreateShaderResourceView(e.buf, &sd, &e.srv);
    e.bytes = bytes;
    e.stripe_hash.assign(stripes, 0);
    e.stripe_frame.assign(stripes, ~0ull);
    e.stripe_changed.assign(stripes, ~0ull);
    e.shadow.resize(stripes);
  }
  hi = std::min(hi, bytes);
  if (lo >= hi) return e.srv;
  const uint32_t s0 = lo / kVbStripe, s1 = (hi - 1) / kVbStripe;
  uint32_t run_start = ~0u;
  auto flush = [&](uint32_t end_stripe) {  // upload stripes [run_start, end_stripe)
    if (run_start == ~0u) return;
    const uint32_t off = run_start * kVbStripe, len = std::min(end_stripe * kVbStripe, bytes) - off;
    VbUpload& u = uploads.emplace_back();
    u.buf = e.buf;
    u.off = off;
    u.data.resize(len / 4);
    SwapCopy(u.data.data(), src + off, len / 4, endian);
    g_prof_bytes_uploaded += len;
    run_start = ~0u;
  };
  for (uint32_t i = s0; i <= s1; ++i) {
    bool dirty = false;
    const uint32_t off = i * kVbStripe;
    const uint32_t len = std::min(kVbStripe, bytes - off);
    if (e.stripe_frame[i] != g_vb_frame) {
      // first use this frame: full content hash
      const uint64_t h = SampleHashBytes(src + off, len);
      const bool first = e.stripe_frame[i] == ~0ull;
      dirty = first || h != e.stripe_hash[i];
      e.stripe_hash[i] = h;
      e.stripe_frame[i] = g_vb_frame;
      // new and rewritten stripes start hot: the game's temporary vertex memory is reused within a
      // frame, so it must be compared on every draw until it has stayed the same for a while
      if (dirty) e.stripe_changed[i] = g_vb_frame;
      const bool hot = e.stripe_changed[i] != ~0ull && g_vb_frame - e.stripe_changed[i] < kVbHotFrames;
      if (hot) {
        if (!e.shadow[i]) e.shadow[i].reset(new uint8_t[kVbStripe]);
        std::memcpy(e.shadow[i].get(), src + off, len);
      } else {
        e.shadow[i].reset();
      }
    } else if (e.shadow[i]) {
      // already verified this frame, but the stripe is hot: compare the bytes this draw reads
      const uint32_t a = std::max(lo, off) - off;
      const uint32_t b = std::min(hi, off + len) - off;
      if (a < b && std::memcmp(e.shadow[i].get() + a, src + off + a, b - a) != 0) {
        std::memcpy(e.shadow[i].get(), src + off, len);
        e.stripe_hash[i] = SampleHashBytes(src + off, len);
        e.stripe_changed[i] = g_vb_frame;
        ++g_prof_vb_midframe;
        dirty = true;
      }
    }
    if (dirty) {
      if (run_start == ~0u) run_start = i;
    } else {
      flush(i);
    }
  }
  flush(s1 + 1);
  return e.srv;
}

void TrimVertexCache() {
  ++g_vb_frame;
  if (g_vb_frame % 600) return;
  for (auto it = g_vb_cache.begin(); it != g_vb_cache.end();) {
    if (g_vb_frame - it->second.last_used > 600) {
      DeferRelease(it->second.srv);  // the render thread may still have draws using it
      DeferRelease(it->second.buf);
      it = g_vb_cache.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Per-frame state
struct FrameStats {
  uint32_t draws = 0, skipped = 0, failed_shader = 0, clears = 0, resolves = 0;
} g_stats;
bool g_inited = false;

bool Init() {
  if (g_inited) return true;
  InstallCrashLogger();
  if (!native::EnsureDevice()) return false;
  g_vb_ring.Create(64u << 20, D3D11_BIND_SHADER_RESOURCE, true);
  g_ib_ring.Create(16u << 20, D3D11_BIND_INDEX_BUFFER, false);
  g_cb_vs = MakeCB(4096);
  g_cb_ps = MakeCB(4096);
  g_cb_draw = MakeCB(sizeof(XeDrawCB));
  std::string err;
  ID3DBlob* gs = Compile(kRectGs, "gs_5_0", 0x1234, err);
  if (gs) {
    native::Device()->CreateGeometryShader(gs->GetBufferPointer(), gs->GetBufferSize(), nullptr, &g_rect_gs);
    gs->Release();
  } else {
    Log("rect GS failed: %s", err.c_str());
  }
  if (ID3DBlob* pgs = Compile(kPointGs, "gs_5_0", 0x5678, err)) {
    native::Device()->CreateGeometryShader(pgs->GetBufferPointer(), pgs->GetBufferSize(), nullptr, &g_point_gs);
    pgs->Release();
  } else {
    Log("point GS failed: %s", err.c_str());
  }
  if (const char* h = std::getenv("Q4_HLE_RT_HEIGHT")) g_rt_height = std::max(64, std::atoi(h));
  g_scale = DesiredScale();
  tex::RefreshSettings();
  RefreshCrtSettings();
  RefreshStereoSettings();
  Log("renderer: internal resolution scale %.3fx", g_scale);
  OutDir();
  g_inited = true;
  Log("renderer: initialised");
  return true;
}

// ---------------------------------------------------------------------------------------------
// Draw
struct DrawArgs {
  uint32_t prim = 4;
  bool indexed = false;
  uint32_t start = 0;  // first index (indexed draws); non-indexed draws start at VGT_INDX_OFFSET
  uint32_t count = 0;
  uint32_t up_va = 0;     // BeginVertices/EndVertices: vertex data written by the CPU (fetch constant 95)
  uint32_t up_bytes = 0;
};

void FillDrawCB(const Regs& R, const uint8_t* base, uint32_t dev, const xenos::TranslatedShader* ps,
                const xenos::TranslatedShader* vs, const BoundTargets& bt, bool& flip, XeDrawCB& cb) {
  std::memset(&cb, 0, sizeof(cb));
  for (int i = 0; i < 8; ++i) cb.bools[i] = Load32(base, dev + guest::dev::kBoolLoopConstants + 4 * i);
  for (int i = 0; i < 32; ++i) {
    const uint32_t l = Load32(base, dev + guest::dev::kBoolLoopConstants + 32 + 4 * i);
    cb.loops[i][0] = l & 0xFF;
    cb.loops[i][1] = (l >> 8) & 0xFF;
    cb.loops[i][2] = uint32_t(int32_t(l << 8) >> 24);
  }
  // Viewport transform: Xenos screen = ndc * scale + offset (per enabled component); map Xenos
  // screen coordinates to D3D11 NDC for a viewport covering the whole render target.
  const uint32_t vte = R(PA_CL_VTE_CNTL);
  const float xs = (vte & 1) ? R.F(PA_CL_VPORT_XSCALE) : 1.0f, xo = (vte & 2) ? R.F(PA_CL_VPORT_XOFFSET) : 0.0f;
  const float ys = (vte & 4) ? R.F(PA_CL_VPORT_YSCALE) : 1.0f, yo = (vte & 8) ? R.F(PA_CL_VPORT_YOFFSET) : 0.0f;
  const float zs = (vte & 16) ? R.F(PA_CL_VPORT_ZSCALE) : 1.0f, zo = (vte & 32) ? R.F(PA_CL_VPORT_ZOFFSET) : 0.0f;
  RenderTarget* any = bt.color[0] ? bt.color[0] : bt.depth;
  // guest pixel units (render targets are g_scale times larger)
  const float W = any ? std::round(float(any->w) / g_scale) : 1280.0f, H = any ? std::round(float(any->h) / g_scale) : 720.0f;
  const bool half_pixel = !(R(PA_SU_VTX_CNTL) & 1);  // D3D9 pixel centres
  const float hp = half_pixel ? 0.5f : 0.0f;
  cb.ndc_scale[0] = 2.0f * xs / W;
  cb.ndc_offset[0] = 2.0f * (xo + hp) / W - 1.0f;
  cb.ndc_scale[1] = -2.0f * ys / H;
  cb.ndc_offset[1] = 1.0f - 2.0f * (yo + hp) / H;
  cb.ndc_scale[2] = zs;
  cb.ndc_offset[2] = zo;
  flip = cb.ndc_scale[1] < 0.0f;  // y mirrored between Xenos and host NDC -> winding flips
  cb.misc[0] = half_pixel ? 1.0f : 0.0f;
  cb.misc[2] = 1.0f / float(g_scale);
  {
    const uint32_t ps_size = R(0x2280), ps_minmax = R(0x2281);
    cb.point[0] = float(ps_size >> 16) * (2.0f / 16.0f);
    cb.point[1] = float(ps_size & 0xFFFF) * (2.0f / 16.0f);
    cb.point[2] = float(ps_minmax & 0xFFFF) * (2.0f / 16.0f);
    cb.point[3] = float(ps_minmax >> 16) * (2.0f / 16.0f);
    cb.point2[0] = 1.0f / std::max(W, 1.0f);
    cb.point2[1] = 1.0f / std::max(H, 1.0f);
  }
  const uint32_t ccl = R(RB_COLORCONTROL);
  cb.alpha[0] = R.F(RB_ALPHA_REF);
  cb.alpha[1] = float(ccl & 7);
  cb.alpha[2] = ((ccl >> 3) & 1) ? 1.0f : 0.0f;
  cb.alpha[3] = ((ccl >> 4) & 1) ? 1.0f : 0.0f;  // alpha to mask
  {
    const uint32_t offs = ccl >> 24;
    std::memcpy(&cb.misc[1], &offs, 4);
  }
  for (int i = 0; i < 32; ++i) {
    if ((!ps || ps->tex_dim[i] == xenos::TexDim::kNone) && (!vs || vs->tex_dim[i] == xenos::TexDim::kNone)) continue;
    uint32_t d[6];
    for (int k = 0; k < 6; ++k) d[k] = Load32(base, dev + guest::dev::kFetchConstants + 24 * i + 4 * k);
    const tex::FetchInfo f = tex::Decode(d);
    uint32_t swz = f.swizzle;
    // A resolve result holds the render target's RGBA; guest memory would hold the texture
    // format's component order (BGRA for ARGB textures), which the fetch swizzle undoes.
    if ((swz & 7) == 2 && ((swz >> 6) & 7) == 0 && f.base && tex::FindResolved(f.base)) {
      uint32_t out = 0;
      for (int c = 0; c < 4; ++c) {
        uint32_t sel = (swz >> (3 * c)) & 7;
        if (sel == 0) sel = 2;
        else if (sel == 2) sel = 0;
        out |= sel << (3 * c);
      }
      swz = out;
    }
    cb.tex[i][0] = swz;
    cb.tex[i][1] = f.signs;
    const float scale = std::ldexp(1.0f, f.exp_adjust);
    std::memcpy(&cb.tex[i][2], &scale, 4);
  }
}

// Shader resource / sampler bindings are cached so that only slots that changed are passed to
// D3D11 (binding 32 + 96 slots per draw dominated the per-draw CPU cost).
struct BindCache {
  ID3D11ShaderResourceView* ps_srv[32] = {};
  ID3D11ShaderResourceView* vs_srv[128] = {};
  ID3D11SamplerState* ps_smp[16] = {};
  ID3D11SamplerState* vs_smp[16] = {};
} g_bind;
bool g_bind_valid = false;

// Last pipeline state set by Draw (skips redundant D3D11 calls). Reset by InvalidateBindings().
struct StateCache {
  ID3D11RenderTargetView* rtv[4] = {};
  ID3D11DepthStencilView* dsv = nullptr;
  UINT nrt = ~0u;
  D3D11_VIEWPORT vp{};
  D3D11_RECT scissor{};
  bool cbs = false;
  ID3D11DepthStencilState* ds = nullptr;
  UINT sref = ~0u;
  ID3D11BlendState* blend = nullptr;
  float bf[4] = {};
  ID3D11RasterizerState* rs = nullptr;
  UINT ib_off = ~0u;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  ID3D11VertexShader* vs = nullptr;
  ID3D11GeometryShader* gs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  bool valid = false;
} g_state;

// Per-frame memo of fetch constant -> (texture view, sampler): the same textures are bound by many
// draws in a frame, and their content is only re-validated once per frame anyway.
struct TexMemo {
  uint32_t d[6];
  uint32_t dim;
  ID3D11ShaderResourceView* srv;
  ID3D11SamplerState* smp;
};
std::unordered_map<uint64_t, TexMemo> g_tex_memo;
uint64_t g_tex_memo_frame = ~0ull;

void LookupTexture(const uint8_t* base, const uint32_t d[6], xenos::TexDim dim, ID3D11ShaderResourceView** srv,
                   ID3D11SamplerState** smp) {
  if (g_tex_memo_frame != g_frame) {
    g_tex_memo.clear();
    g_tex_memo_frame = g_frame;
  }
  uint64_t h = 1469598103934665603ull ^ uint64_t(dim);
  for (int k = 0; k < 6; ++k) h = (h ^ d[k]) * 1099511628211ull;
  auto it = g_tex_memo.find(h);
  if (it != g_tex_memo.end() && it->second.dim == uint32_t(dim) && std::memcmp(it->second.d, d, 24) == 0) {
    *srv = it->second.srv;
    *smp = it->second.smp;
    return;
  }
  const tex::FetchInfo f = tex::Decode(d);
  TexMemo m;
  std::memcpy(m.d, d, 24);
  m.dim = uint32_t(dim);
  m.srv = tex::GetTexture(base, f, dim, g_frame);
  m.smp = tex::GetSampler(f);
  g_tex_memo[h] = m;
  *srv = m.srv;
  *smp = m.smp;
}

void InvalidateBindings() {
  g_bind_valid = false;
  g_state.valid = false;
}

template <typename T, typename SetFn>
void BindChanged(T** cur, T* const* want, int first, int n, SetFn set) {
  int lo = -1, hi = -1;
  for (int i = 0; i < n; ++i) {
    if (!g_bind_valid || cur[first + i] != want[i]) {
      if (lo < 0) lo = i;
      hi = i;
    }
  }
  if (lo < 0) return;
  set(UINT(first + lo), UINT(hi - lo + 1), want + lo);
  for (int i = lo; i <= hi; ++i) cur[first + i] = want[i];
}

// Q4_HLE_STEP_DUMP=lo-hi: in the captured frame, write colour target 0 after each draw in the
// range (half resolution, 8-bit formats only) as step_NNNN.bmp, to find the draw that breaks.
uint32_t g_step_lo = 1, g_step_hi = 0, g_step_index = 0;
bool g_step_parsed = false;

void StepDump(RenderTarget* rt, uint32_t index) {
  if (!rt || !rt->tex || rt->depth) return;
  D3D11_TEXTURE2D_DESC td;
  rt->tex->GetDesc(&td);
  if (td.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS && td.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
      td.Format != DXGI_FORMAT_B8G8R8A8_TYPELESS && td.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
    return;
  const bool bgra = td.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS || td.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
  const uint32_t w = std::min<uint32_t>(td.Width, Sc(1280u)), h = std::min<uint32_t>(td.Height, Sc(720u));
  td.Usage = D3D11_USAGE_STAGING;
  td.BindFlags = 0;
  td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  td.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  if (FAILED(native::Device()->CreateTexture2D(&td, nullptr, &st))) return;
  auto* ctx = native::Context();
  ctx->CopyResource(st, rt->tex);
  D3D11_MAPPED_SUBRESOURCE ms;
  if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &ms))) {
    const uint32_t step = std::max<uint32_t>(1, Sc(2u)), ow = w / step, oh = h / step;
    std::vector<uint8_t> px(size_t(ow) * oh * 4);
    for (uint32_t y = 0; y < oh; ++y)
      for (uint32_t x = 0; x < ow; ++x)
        std::memcpy(&px[(size_t(y) * ow + x) * 4],
                    static_cast<const uint8_t*>(ms.pData) + size_t(y * step) * ms.RowPitch + size_t(x * step) * 4, 4);
    ctx->Unmap(st, 0);
    char name[64];
    std::snprintf(name, sizeof(name), "/step_%04u.bmp", index);
    native::WriteBmp(OutDir() + name, px.data(), ow, oh, ow * 4, bgra);
  }
  st->Release();
}

// ---------------------------------------------------------------------------------------------
// Render thread.
//
// The hooked D3D calls run on the game's thread. Everything that reads guest memory which the
// game may change right after the call (the device's register/constant shadows, index data,
// vertex data, pointer arguments) is captured there into a command; the render thread then does
// the rest (render targets, textures, constant uploads, state, D3D11 calls) in order. ID3D11Device
// methods are thread-safe, so the game thread may create resources; the immediate context is used
// only by the render thread. Resources owned by game-thread caches are released through the queue
// (after every command that may still use them).

// Device shadow ranges the renderer reads (see guest.h).
struct DevRange {
  uint32_t off, len;
};
constexpr DevRange kDevRanges[] = {
    {0x0480, 0x0300},  // fetch constants
    {0x2780, 0x00B0},  // bool / loop constants
    {0x2CC0, 0x0210},  // register shadows
    {0x30F4, 0x00A0},  // index buffer, render targets, depth, streams
    {0x3290, 0x0008},  // bound pixel / vertex shader
};
constexpr uint32_t kDevSnapBytes = 0x300 + 0xB0 + 0x210 + 0xA0 + 0x8;
constexpr uint32_t kDevImageBytes = 0x3300;

// VS + PS float constants (raw, big-endian), shared by consecutive draws that use the same values.
struct ConstBlock {
  uint32_t raw[2048];
  uint64_t id;  // unique per block (a freed block's address can be reused by a new one)
};

struct Cmd {
  enum Type : uint8_t { kDraw, kClear, kResolve, kEndFrame, kRelease } type = kDraw;
  uint32_t dev = 0;
  uint8_t snap[kDevSnapBytes];
  std::shared_ptr<const ConstBlock> consts;
  // draw
  ShaderEntry* vs = nullptr;
  ShaderEntry* ps = nullptr;
  D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  bool rect = false, use_draw = false;
  DXGI_FORMAT ib_fmt = DXGI_FORMAT_R32_UINT;
  uint32_t draw_count = 0;
  RawVec<uint8_t> indices;
  ID3D11ShaderResourceView* vb_cached[96] = {};
  struct RingVb {
    uint32_t slot;
    RawVec<uint32_t> data;
  };
  std::vector<RingVb> vb_ring;
  std::vector<VbUpload> vb_uploads;
  int32_t scissor[4] = {};
  uint32_t step_index = ~0u;
  // clear / resolve (pointer arguments dereferenced on the game thread)
  PPCContext args{};
  std::vector<D3D11_RECT> rects;
  bool has_src_rect = false, has_dest_pt = false, has_dest_tex = false, has_clear_color = false;
  uint32_t src_rect[4] = {}, dest_pt[2] = {}, dest_fetch[6] = {};
  float clear_color[4] = {};
  // end of frame
  uint32_t front_fetch[6] = {};
  bool has_front = false;
  // release
  std::vector<IUnknown*> release;
};

struct Queue {
  std::mutex mu;
  std::condition_variable cv, done_cv;
  std::vector<std::unique_ptr<Cmd>> pending;
  std::vector<std::unique_ptr<Cmd>> free_list;
  uint64_t frames_submitted = 0;
  std::atomic<uint64_t> frames_done{0};
  uint64_t cmds_pushed = 0;           // game thread
  std::atomic<uint64_t> cmds_done{0};  // render thread
  bool running = false;
} g_q;
std::thread g_render_thread;
uint8_t* volatile g_q_base = nullptr;  // guest memory base (set by the game thread before the first command)
std::vector<std::unique_ptr<Cmd>> g_batch;  // game thread: commands not yet handed over

std::unique_ptr<Cmd> NewCmd(Cmd::Type t) {
  std::unique_ptr<Cmd> c;
  {
    std::lock_guard<std::mutex> lk(g_q.mu);
    if (!g_q.free_list.empty()) {
      c = std::move(g_q.free_list.back());
      g_q.free_list.pop_back();
    }
  }
  if (!c) c = std::make_unique<Cmd>();
  // Commands are recycled: every field a capture may leave unset must be reset here.
  c->type = t;
  c->dev = 0;
  c->topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
  c->rect = false;
  c->use_draw = false;
  c->ib_fmt = DXGI_FORMAT_R32_UINT;
  c->draw_count = 0;
  std::memset(c->scissor, 0, sizeof(c->scissor));
  c->args = PPCContext{};
  std::memset(c->src_rect, 0, sizeof(c->src_rect));
  std::memset(c->dest_pt, 0, sizeof(c->dest_pt));
  std::memset(c->dest_fetch, 0, sizeof(c->dest_fetch));
  std::memset(c->clear_color, 0, sizeof(c->clear_color));
  std::memset(c->front_fetch, 0, sizeof(c->front_fetch));
  c->consts.reset();
  c->vs = c->ps = nullptr;
  c->indices.clear();
  std::memset(c->vb_cached, 0, sizeof(c->vb_cached));
  c->vb_ring.clear();
  c->vb_uploads.clear();
  c->step_index = ~0u;
  c->rects.clear();
  c->has_src_rect = c->has_dest_pt = c->has_dest_tex = c->has_clear_color = c->has_front = false;
  c->release.clear();
  return c;
}

void RenderThreadMain();

// Hands the batched commands to the render thread (once per draw batch / frame).
void Flush() {
  if (g_batch.empty()) return;
  {
    std::lock_guard<std::mutex> lk(g_q.mu);
    for (auto& c : g_batch) g_q.pending.push_back(std::move(c));
  }
  g_batch.clear();
  g_q.cv.notify_one();
}

void Push(std::unique_ptr<Cmd> c) {
  if (!g_q.running) {
    g_q.running = true;
    g_render_thread = std::thread(RenderThreadMain);
    g_render_thread.detach();  // runs for the life of the process
  }
  g_batch.push_back(std::move(c));
  static const bool sync = [] {
    const char* e = std::getenv("Q4_HLE_SYNC");
    return e && *e == '1';
  }();
  if (sync) {
    // diagnostic: run every command before the game continues
    const uint64_t target = g_q.cmds_pushed + g_batch.size();
    g_q.cmds_pushed = target;
    Flush();
    std::unique_lock<std::mutex> lk(g_q.mu);
    g_q.done_cv.wait(lk, [&] { return g_q.cmds_done.load() >= target; });
    return;
  }
  g_q.cmds_pushed += 1;
  if (g_batch.size() >= 32) Flush();
}

// Releases a resource after every command queued so far has executed.
void DeferRelease(IUnknown* p) {
  if (!p) return;
  auto c = NewCmd(Cmd::kRelease);
  c->release.push_back(p);
  Push(std::move(c));
}

// Game thread: device shadow snapshot + float constants (shared while unchanged).
std::shared_ptr<const ConstBlock> g_last_consts;
// Constant blocks are recycled (8 KB each; a few hundred change per frame).
std::mutex g_cb_pool_mu;
std::vector<ConstBlock*> g_cb_pool;
std::shared_ptr<ConstBlock> NewConstBlock() {
  ConstBlock* b = nullptr;
  {
    std::lock_guard<std::mutex> lk(g_cb_pool_mu);
    if (!g_cb_pool.empty()) {
      b = g_cb_pool.back();
      g_cb_pool.pop_back();
    }
  }
  if (!b) b = new ConstBlock;
  return std::shared_ptr<ConstBlock>(b, [](ConstBlock* p) {
    std::lock_guard<std::mutex> lk(g_cb_pool_mu);
    if (g_cb_pool.size() < 1024) g_cb_pool.push_back(p);
    else delete p;
  });
}
void SnapshotDevice(const uint8_t* base, uint32_t dev, Cmd& c) {
  c.dev = dev;
  uint8_t* d = c.snap;
  for (const auto& r : kDevRanges) {
    std::memcpy(d, guest::Ptr(base, dev + r.off), r.len);
    d += r.len;
  }
  const uint8_t* consts = guest::Ptr(base, dev + guest::dev::kVsFloatConstants);
  if (!g_last_consts || std::memcmp(g_last_consts->raw, consts, sizeof(g_last_consts->raw)) != 0) {
    static uint64_t next_id = 0;
    auto b = NewConstBlock();
    std::memcpy(b->raw, consts, sizeof(b->raw));
    b->id = ++next_id;
    g_last_consts = std::move(b);
  }
  c.consts = g_last_consts;
}

// Render thread: rebuilds the device shadows in a local image. Returns a base pointer such that
// Load32(dbase, dev + offset) reads the image.
alignas(16) uint8_t g_dev_image[kDevImageBytes];
uint64_t g_image_consts = 0;
const uint8_t* ApplySnapshot(const Cmd& c) {
  const uint8_t* s = c.snap;
  for (const auto& r : kDevRanges) {
    std::memcpy(g_dev_image + r.off, s, r.len);
    s += r.len;
  }
  if (c.consts && c.consts->id != g_image_consts) {
    std::memcpy(g_dev_image + guest::dev::kVsFloatConstants, c.consts->raw, sizeof(c.consts->raw));
    g_image_consts = c.consts->id;
  }
  return reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(g_dev_image) - uintptr_t(c.dev));
}

// Game-thread profile (capture).
uint64_t g_cap_prof = 0;

// ---------------------------------------------------------------------------------------------
// Draw: capture on the game thread
void Draw(uint8_t* base, uint32_t dev, const DrawArgs& a) {
  if (!Init()) return;
  g_q_base = base;
  if (!g_step_parsed) {
    g_step_parsed = true;
    if (const char* e = std::getenv("Q4_HLE_STEP_DUMP")) {
      g_step_lo = uint32_t(std::strtoul(e, nullptr, 10));
      const char* dash = std::strchr(e, '-');
      g_step_hi = dash ? uint32_t(std::strtoul(dash + 1, nullptr, 10)) : g_step_lo;
    }
  }
  const uint64_t t0 = Now();
  struct ProfCap {
    uint64_t t0;
    ~ProfCap() { g_cap_prof += Now() - t0; }
  } prof_cap{t0};
  const bool capturing = capture::Active();
  if (!capturing) g_step_index = 0;
  const uint32_t step_index = capturing ? g_step_index++ : ~0u;
  ++g_stats.draws;
  Regs R{base, dev};

  // shaders
  const uint32_t vs_obj = Load32(base, dev + guest::dev::kVertexShaderBound);
  const uint32_t ps_obj = Load32(base, dev + guest::dev::kPixelShaderBound);
  if (!vs_obj) {
    ++g_stats.skipped;
    return;
  }
  const uint32_t pc = R(SQ_PROGRAM_CNTL), misc = R(SQ_CONTEXT_MISC);
  xenos::TranslateOptions vopt;
  ShaderEntry* vs = GetShader(base, Load32(base, vs_obj + guest::obj::kVsUcodeAddr),
                              Load32(base, vs_obj + guest::obj::kVsUcodeSize), false, vopt);
  ShaderEntry* ps = nullptr;
  if (ps_obj) {
    xenos::TranslateOptions popt;
    popt.param_gen_reg = ((pc >> 18) & 1) ? int((misc >> 8) & 0xFF) : -1;
    popt.interpolator_count = std::min<uint32_t>(((pc >> 20) & 0xF) + 1, ((pc >> 8) & 0x3F) + 1);
    ps = GetShader(base, Load32(base, ps_obj + guest::obj::kPsUcodeAddr) & ~1u,
                   Load32(base, ps_obj + guest::obj::kPsUcodeSize), true, popt);
  }
  if (!vs || (ps_obj && !ps)) {
    ++g_stats.failed_shader;
    return;
  }

  bool needs_list, rect;
  const D3D11_PRIMITIVE_TOPOLOGY topo = Topology(a.prim, needs_list, rect);
  if (topo == D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED || !a.count) {
    ++g_stats.skipped;
    return;
  }
  auto cmd = NewCmd(Cmd::kDraw);
  Cmd& c = *cmd;
  c.vs = vs;
  c.ps = ps;
  c.topo = topo;
  c.rect = rect;
  c.step_index = step_index;
  std::memcpy(c.scissor, g_scissor, sizeof(c.scissor));

  // Indices: list/strip types are copied byte-swapped (16-bit kept 16-bit); non-indexed draws use
  // Draw; primitive types D3D11 lacks are expanded; the base vertex is added to the indices.
  const uint32_t base_vertex = R(VGT_INDX_OFFSET) & 0xFFFFFF;
  uint32_t max_index = 0, min_index = ~0u;
  if (a.indexed) {
    const uint32_t ib = Load32(base, dev + guest::dev::kIndexBuffer);
    if (!ib) {
      ++g_stats.skipped;
      return;
    }
    const bool is32 = (Load32(base, ib) & 0x80000000u) != 0;
    const uint32_t ib_addr = Load32(base, ib + 0xC);
    const uint8_t* p = guest::Ptr(base, ib_addr + a.start * (is32 ? 4 : 2));
    if (!needs_list && !base_vertex) {
      c.draw_count = a.count;
      if (is32) {
        c.ib_fmt = DXGI_FORMAT_R32_UINT;
        c.indices.resize(size_t(a.count) * 4);
        uint32_t* d32 = reinterpret_cast<uint32_t*>(c.indices.data());
        uint32_t lo = ~0u, hi = 0;
        for (uint32_t i = 0; i < a.count; ++i) {
          uint32_t v;
          std::memcpy(&v, p + 4 * size_t(i), 4);
          v = __builtin_bswap32(v);
          d32[i] = v;
          lo = std::min(lo, v);
          hi = std::max(hi, v);
        }
        min_index = lo, max_index = hi;
      } else {
        c.indices.resize((size_t(a.count) * 2 + 3) & ~size_t(3));
        uint16_t* d16 = reinterpret_cast<uint16_t*>(c.indices.data());
        uint32_t lo = 0xFFFF, hi = 0;
        for (uint32_t i = 0; i < a.count; ++i) {
          uint16_t v;
          std::memcpy(&v, p + 2 * size_t(i), 2);
          v = __builtin_bswap16(v);
          d16[i] = v;
          lo = std::min<uint32_t>(lo, v);
          hi = std::max<uint32_t>(hi, v);
        }
        min_index = lo, max_index = hi;
        c.ib_fmt = DXGI_FORMAT_R16_UINT;
      }
    } else {
      static std::vector<uint32_t> idx, expanded;
      idx.resize(a.count);
      for (uint32_t i = 0; i < a.count; ++i) {
        idx[i] = ReadIndex(p, i, is32);
        max_index = std::max(max_index, idx[i]);
        min_index = std::min(min_index, idx[i]);
      }
      if (needs_list) ExpandIndices(a.prim, idx, expanded);
      else expanded = idx;
      if (base_vertex)
        for (auto& v : expanded) v += base_vertex;
      c.indices.resize(expanded.size() * 4);
      std::memcpy(c.indices.data(), expanded.data(), expanded.size() * 4);
      c.draw_count = uint32_t(expanded.size());
      c.ib_fmt = DXGI_FORMAT_R32_UINT;
    }
  } else {
    max_index = a.count - 1;
    min_index = 0;
    if (!needs_list && !base_vertex) {
      c.use_draw = true;
      c.draw_count = a.count;
    } else {
      static std::vector<uint32_t> idx, expanded;
      idx.resize(a.count);
      for (uint32_t i = 0; i < a.count; ++i) idx[i] = i + base_vertex;
      if (needs_list) ExpandIndices(a.prim, idx, expanded);
      else expanded = idx;
      c.indices.resize(expanded.size() * 4);
      std::memcpy(c.indices.data(), expanded.data(), expanded.size() * 4);
      c.draw_count = uint32_t(expanded.size());
      c.ib_fmt = DXGI_FORMAT_R32_UINT;
    }
  }
  if (!c.draw_count) {
    ++g_stats.skipped;
    return;
  }

  // vertex buffers (guest data is read here; uploads happen on the render thread)
  for (uint32_t k = 0; k < 96; ++k) {
    if (!(vs->info.vfetch_mask[k >> 5] & (1u << (k & 31)))) continue;
    const uint32_t d0 = Load32(base, dev + guest::dev::kFetchConstants + 8 * k);
    const uint32_t d1 = Load32(base, dev + guest::dev::kFetchConstants + 8 * k + 4);
    uint32_t addr = d0 & 0xFFFFFFFCu, size = ((d1 >> 2) & 0xFFFFFF) * 4, endian = d1 & 3;
    const bool up = a.up_va && k == 95;
    if (up) size = a.up_bytes, endian = 2;
    uint32_t need = ((max_index + base_vertex) * vs->info.vf_stride_dw[k] + vs->info.vf_extent_dw[k]) * 4;
    need = std::min(std::max(need, 16u), std::max(size, 16u));
    need = (need + 15) & ~15u;
    const uint8_t* src = up ? guest::Ptr(base, a.up_va) : guest::PhysPtr(base, addr);
    if (!up) {
      const uint32_t stride = vs->info.vf_stride_dw[k] * 4;
      const uint32_t lo_b = (min_index + base_vertex) * stride;
      const uint32_t hi_b = (max_index + base_vertex) * stride + vs->info.vf_extent_dw[k] * 4;
      if (ID3D11ShaderResourceView* cached = CachedVertexBuffer(src, addr, size, endian, lo_b, hi_b, c.vb_uploads)) {
        c.vb_cached[k] = cached;
        continue;
      }
    }
    Cmd::RingVb& rv = c.vb_ring.emplace_back();
    rv.slot = k;
    rv.data.resize(need / 4);
    SwapCopy(rv.data.data(), src, need / 4, endian);
  }

  SnapshotDevice(base, dev, c);
  Push(std::move(cmd));
}

// ---------------------------------------------------------------------------------------------
// Draw: execution on the render thread
void StereoTrackDraw(const Regs& R, const BoundTargets& bt);

void ExecuteDraw(uint8_t* base, Cmd& c) {
  ProfScope prof_total(kProfDraw);
  uint64_t lap = Now();
  auto Lap = [&](int sec) {
    const uint64_t t = Now();
    g_prof[sec] += t - lap;
    lap = t;
  };
  struct StepAfter {
    uint32_t index;
    RenderTarget* rt = nullptr;
    ~StepAfter() {
      if (rt && index >= g_step_lo && index <= g_step_hi) StepDump(rt, index);
    }
  } step_after{c.step_index};
  auto* ctx = native::Context();
  const uint8_t* dbase = ApplySnapshot(c);
  const uint32_t dev = c.dev;
  Regs R{dbase, dev};
  ShaderEntry* vs = c.vs;
  ShaderEntry* ps = c.ps;

  // indices
  uint32_t ib_off = 0;
  if (!c.use_draw) {
    uint8_t* dst = g_ib_ring.Alloc(uint32_t(c.indices.size()), 4, ib_off);
    if (!dst) return;
    std::memcpy(dst, c.indices.data(), c.indices.size());
    g_ib_ring.Unmap();
  }
  Lap(kProfIndex);

  // vertex buffers
  for (const auto& u : c.vb_uploads) {
    D3D11_BOX box{u.off, 0, 0, u.off + uint32_t(u.data.size() * 4), 1, 1};
    ctx->UpdateSubresource(u.buf, 0, &box, u.data.data(), 0, 0);
  }
  ID3D11ShaderResourceView* vb_srvs[96];
  std::memcpy(vb_srvs, c.vb_cached, sizeof(vb_srvs));
  ID3D11ShaderResourceView* created[96];
  uint32_t ncreated = 0;
  for (const auto& rv : c.vb_ring) {
    const uint32_t need = uint32_t(rv.data.size() * 4);
    uint32_t off = 0;
    uint8_t* dst = g_vb_ring.Alloc(need, 256, off);
    if (!dst) continue;
    std::memcpy(dst, rv.data.data(), need);
    g_vb_ring.Unmap();
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
    sd.BufferEx.FirstElement = off / 4;
    sd.BufferEx.NumElements = need / 4;
    sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
    ID3D11ShaderResourceView* srv = nullptr;
    native::Device()->CreateShaderResourceView(g_vb_ring.buf, &sd, &srv);
    vb_srvs[rv.slot] = srv;
    if (srv) created[ncreated++] = srv;
  }
  Lap(kProfVb);

  // render targets
  BoundTargets bt = CurrentTargets(R);
  ID3D11RenderTargetView* rtvs[4] = {};
  UINT nrt = 0;
  for (int i = 0; i < 4; ++i)
    if (bt.color[i]) rtvs[i] = bt.color[i]->rtv, nrt = i + 1;
  if (ps && !(ps->info.color_targets & 1) && !ps->info.writes_depth) {
    // depth/stencil-only pixel shader
  }
  {
    ID3D11DepthStencilView* dsv = bt.depth ? bt.depth->dsv : nullptr;
    if (!g_state.valid || g_state.nrt != nrt || g_state.dsv != dsv || std::memcmp(g_state.rtv, rtvs, sizeof(rtvs)) != 0) {
      ctx->OMSetRenderTargets(nrt, rtvs, dsv);
      g_state.nrt = nrt;
      g_state.dsv = dsv;
      std::memcpy(g_state.rtv, rtvs, sizeof(rtvs));
    }
  }
  StereoTrackDraw(R, bt);
  step_after.rt = bt.color[0];
  RenderTarget* any = bt.color[0] ? bt.color[0] : bt.depth;
  D3D11_VIEWPORT vp{0, 0, any ? float(any->w) : 1280.0f, any ? float(any->h) : 720.0f, 0.0f, 1.0f};
  if (!g_state.valid || std::memcmp(&g_state.vp, &vp, sizeof(vp)) != 0) {
    ctx->RSSetViewports(1, &vp);
    g_state.vp = vp;
  }
  {
    D3D11_RECT sr{std::max<int32_t>(0, Sc(int32_t(c.scissor[0]))), std::max<int32_t>(0, Sc(int32_t(c.scissor[1]))),
                  std::min<int32_t>(Sc(int32_t(c.scissor[2])), LONG(vp.Width)),
                  std::min<int32_t>(Sc(int32_t(c.scissor[3])), LONG(vp.Height))};
    if (!g_state.valid || std::memcmp(&g_state.scissor, &sr, sizeof(sr)) != 0) {
      ctx->RSSetScissorRects(1, &sr);
      g_state.scissor = sr;
    }
  }

  // constants
  XeDrawCB cb;
  bool flip = false;
  FillDrawCB(R, dbase, dev, ps ? &ps->info : nullptr, &vs->info, bt, flip, cb);
  Lap(kProfSubmit);
  {
    // Constant buffers are only re-uploaded when their contents changed since the last draw.
    ProfScope ps_(kProfConst);
    static XeDrawCB last_cb;
    static bool have_last = false;
    if (!have_last || std::memcmp(&last_cb, &cb, sizeof(cb)) != 0) {
      UpdateCB(g_cb_draw, &cb, sizeof(cb));
      last_cb = cb;
      ++g_prof_cb_uploads;
    }
    // compare the raw guest words first; byte-swap and upload only when they changed
    static uint32_t last_vs[1024], last_ps[1024];
    uint32_t c[1024];
    const uint32_t* g = reinterpret_cast<const uint32_t*>(dbase + dev + guest::dev::kVsFloatConstants);
    if (!have_last || std::memcmp(g, last_vs, sizeof(c)) != 0) {
      std::memcpy(last_vs, g, sizeof(c));
      for (int i = 0; i < 1024; ++i) c[i] = __builtin_bswap32(g[i]);
      UpdateCB(g_cb_vs, c, sizeof(c));
      ++g_prof_cb_uploads;
    }
    g = reinterpret_cast<const uint32_t*>(dbase + dev + guest::dev::kPsFloatConstants);
    if (!have_last || std::memcmp(g, last_ps, sizeof(c)) != 0) {
      std::memcpy(last_ps, g, sizeof(c));
      for (int i = 0; i < 1024; ++i) c[i] = __builtin_bswap32(g[i]);
      UpdateCB(g_cb_ps, c, sizeof(c));
      ++g_prof_cb_uploads;
    }
    have_last = true;
  }
  lap = Now();
  ID3D11Buffer* vcb[2] = {g_cb_vs, g_cb_draw};
  ID3D11Buffer* pcb[2] = {g_cb_ps, g_cb_draw};
  if (!g_state.valid || !g_state.cbs) {
    ctx->VSSetConstantBuffers(0, 2, vcb);
    ctx->PSSetConstantBuffers(0, 2, pcb);
    ctx->GSSetConstantBuffers(0, 2, vcb);
    g_state.cbs = true;
  }

  // textures
  auto bind_textures = [&](const xenos::TranslatedShader& info, bool pixel) {
    ID3D11ShaderResourceView* srvs[32] = {};
    ID3D11SamplerState* smps[16] = {};
    bool any_tex = false;
    for (int i = 0; i < 32; ++i) {
      if (info.tex_dim[i] == xenos::TexDim::kNone) continue;
      uint32_t d[6];
      for (int k = 0; k < 6; ++k) d[k] = Load32(dbase, dev + guest::dev::kFetchConstants + 24 * i + 4 * k);
      ID3D11SamplerState* smp = nullptr;
      LookupTexture(base, d, info.tex_dim[i], &srvs[i], &smp);
      if (info.sampler_slot[i] >= 0) smps[info.sampler_slot[i]] = smp;
      any_tex = true;
    }
    if (!any_tex) return;
    // Render targets are never sampled directly (resolves copy them), so there is no SRV/RTV
    // hazard and slots can stay bound between draws.
    if (pixel) {
      BindChanged(g_bind.ps_srv, srvs, 0, 32, [&](UINT a, UINT n, ID3D11ShaderResourceView* const* v) { ctx->PSSetShaderResources(a, n, v); });
      BindChanged(g_bind.ps_smp, smps, 0, 16, [&](UINT a, UINT n, ID3D11SamplerState* const* v) { ctx->PSSetSamplers(a, n, v); });
    } else {
      BindChanged(g_bind.vs_srv, srvs, 0, 32, [&](UINT a, UINT n, ID3D11ShaderResourceView* const* v) { ctx->VSSetShaderResources(a, n, v); });
      BindChanged(g_bind.vs_smp, smps, 0, 16, [&](UINT a, UINT n, ID3D11SamplerState* const* v) { ctx->VSSetSamplers(a, n, v); });
    }
  };
  bind_textures(vs->info, false);
  if (ps) bind_textures(ps->info, true);
  BindChanged(g_bind.vs_srv, vb_srvs, 32, 96, [&](UINT a, UINT n, ID3D11ShaderResourceView* const* v) { ctx->VSSetShaderResources(a, n, v); });
  g_bind_valid = true;
  Lap(kProfTex);

  // state (redundant D3D11 calls skipped)
  uint32_t sref = 0;
  {
    ID3D11DepthStencilState* ds = GetDepthStencil(R, sref);
    if (!g_state.valid || g_state.ds != ds || g_state.sref != sref) {
      ctx->OMSetDepthStencilState(ds, sref);
      g_state.ds = ds;
      g_state.sref = sref;
    }
    const float bf[4] = {R.F(RB_BLEND_RED), R.F(RB_BLEND_RED + 1), R.F(RB_BLEND_RED + 2), R.F(RB_BLEND_RED + 3)};
    ID3D11BlendState* bs = GetBlend(R);
    if (!g_state.valid || g_state.blend != bs || std::memcmp(g_state.bf, bf, sizeof(bf)) != 0) {
      ctx->OMSetBlendState(bs, bf, 0xFFFFFFFF);
      g_state.blend = bs;
      std::memcpy(g_state.bf, bf, sizeof(bf));
    }
    ID3D11RasterizerState* rs = GetRaster(R, flip);
    if (!g_state.valid || g_state.rs != rs) {
      ctx->RSSetState(rs);
      g_state.rs = rs;
    }
  }
  if (!c.use_draw && (!g_state.valid || g_state.ib_off != UINT(c.ib_fmt))) {
    // the whole ring stays bound; draws select their indices with StartIndexLocation
    ctx->IASetInputLayout(nullptr);
    ctx->IASetIndexBuffer(g_ib_ring.buf, c.ib_fmt, 0);
    g_state.ib_off = UINT(c.ib_fmt);
  }
  if (!g_state.valid || g_state.topo != c.topo) {
    ctx->IASetPrimitiveTopology(c.topo);
    g_state.topo = c.topo;
  }
  const bool points = c.topo == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
  ID3D11GeometryShader* gs = c.rect ? g_rect_gs : points ? g_point_gs : nullptr;
  ID3D11PixelShader* pso = ps ? ps->ps : nullptr;
  if (!g_state.valid || g_state.vs != vs->vs) ctx->VSSetShader(vs->vs, nullptr, 0), g_state.vs = vs->vs;
  if (!g_state.valid || g_state.gs != gs) ctx->GSSetShader(gs, nullptr, 0), g_state.gs = gs;
  if (!g_state.valid || g_state.ps != pso) ctx->PSSetShader(pso, nullptr, 0), g_state.ps = pso;
  g_state.valid = true;
  // SV_VertexID = index (the base vertex, if any, was added to the indices above)
  if (c.use_draw)
    ctx->Draw(c.draw_count, 0);
  else
    ctx->DrawIndexed(c.draw_count, ib_off / (c.ib_fmt == DXGI_FORMAT_R16_UINT ? 2 : 4), 0);

  for (uint32_t i = 0; i < ncreated; ++i) created[i]->Release();  // still referenced by the context while bound
  Lap(kProfSubmit);
}

// ---------------------------------------------------------------------------------------------
// Clear / Resolve
// Game thread: capture a Clear (device shadows and the rectangle list).
void Clear(uint8_t* base, const PPCContext& c) {
  if (!Init()) return;
  g_q_base = base;
  auto cmd = NewCmd(Cmd::kClear);
  cmd->args = c;
  const uint32_t count = c.r4.u32, rects = c.r5.u32;
  if (count && rects && count < 256) {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t p = rects + 16 * i;
      cmd->rects.push_back({LONG(Load32(base, p)), LONG(Load32(base, p + 4)), LONG(Load32(base, p + 8)), LONG(Load32(base, p + 12))});
    }
  }
  SnapshotDevice(base, c.r3.u32, *cmd);
  Push(std::move(cmd));
}

void ExecuteClear(Cmd& cmd) {
  ProfScope prof_(kProfClearResolve);
  ++g_stats.clears;
  const PPCContext& c = cmd.args;
  const uint32_t dev = c.r3.u32, flags = c.r6.u32, color = c.r7.u32, stencil = c.r8.u32;
  const float z = float(c.f1.f64);
  Regs R{ApplySnapshot(cmd), dev};
  BoundTargets bt = CurrentTargets(R);
  auto* ctx = native::Context();
  const float col[4] = {((color >> 16) & 255) / 255.0f, ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f,
                        (color >> 24) / 255.0f};
  std::vector<D3D11_RECT> rl = cmd.rects;
  for (auto& r : rl) r.left = Sc(int32_t(r.left)), r.top = Sc(int32_t(r.top)), r.right = Sc(int32_t(r.right)), r.bottom = Sc(int32_t(r.bottom));
  ID3D11DeviceContext1* ctx1 = nullptr;
  if (!rl.empty()) ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&ctx1));
  for (int i = 0; i < 4; ++i) {
    if (!(flags & (1u << i)) || !bt.color[i]) continue;
    if (ctx1)
      ctx1->ClearView(bt.color[i]->rtv, col, rl.data(), UINT(rl.size()));
    else
      ctx->ClearRenderTargetView(bt.color[i]->rtv, col);
  }
  SafeRelease(ctx1);
  if ((flags & 0x30) && bt.depth) {
    UINT f = ((flags & 0x10) ? D3D11_CLEAR_DEPTH : 0) | ((flags & 0x20) ? D3D11_CLEAR_STENCIL : 0);
    ctx->ClearDepthStencilView(bt.depth->dsv, f, std::clamp(z, 0.0f, 1.0f), UINT8(stencil));
    if (flags & 0x10) g_depth_clear = std::clamp(z, 0.0f, 1.0f);
  }
}

struct ResolvedTex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint32_t w = 0, h = 0;
  DXGI_FORMAT family = DXGI_FORMAT_UNKNOWN;
};
std::unordered_map<uint32_t, ResolvedTex> g_resolve_targets;

// Game thread: capture a Resolve (device shadows and the pointer arguments).
void Resolve(uint8_t* base, const PPCContext& c) {
  if (!Init()) return;
  g_q_base = base;
  auto cmd = NewCmd(Cmd::kResolve);
  cmd->args = c;
  const uint32_t src_rect = c.r5.u32, dest_tex = c.r6.u32, dest_pt = c.r7.u32, clear_color_ptr = c.r10.u32;
  if (dest_tex) {
    cmd->has_dest_tex = true;
    for (int k = 0; k < 6; ++k) cmd->dest_fetch[k] = Load32(base, dest_tex + guest::obj::kTextureFetch + 4 * k);
  }
  if (src_rect) {
    cmd->has_src_rect = true;
    for (int k = 0; k < 4; ++k) cmd->src_rect[k] = Load32(base, src_rect + 4 * k);
  }
  if (dest_pt) {
    cmd->has_dest_pt = true;
    for (int k = 0; k < 2; ++k) cmd->dest_pt[k] = Load32(base, dest_pt + 4 * k);
  }
  if (clear_color_ptr) {
    cmd->has_clear_color = true;
    for (int i = 0; i < 4; ++i) cmd->clear_color[i] = guest::LoadF(base, clear_color_ptr + 4 * i);
  }
  SnapshotDevice(base, c.r3.u32, *cmd);
  Push(std::move(cmd));
}

void ExecuteResolve(Cmd& cmd) {
  ProfScope prof_(kProfClearResolve);
  g_tex_memo.clear();  // a resolve can replace what a texture address refers to
  ++g_stats.resolves;
  const PPCContext& c = cmd.args;
  const uint32_t dev = c.r3.u32, flags = c.r4.u32;
  Regs R{ApplySnapshot(cmd), dev};
  BoundTargets bt = CurrentTargets(R);
  RenderTarget* src = (flags & 4) ? bt.depth : bt.color[(flags >> 0) & 3];
  if (!src) return;
  auto* ctx = native::Context();
  if (cmd.has_dest_tex) {
    const tex::FetchInfo f = tex::Decode(cmd.dest_fetch);
    uint32_t x1 = 0, y1 = 0, x2 = f.width, y2 = f.height;
    if (cmd.has_src_rect) {
      x1 = cmd.src_rect[0];
      y1 = cmd.src_rect[1];
      x2 = cmd.src_rect[2];
      y2 = cmd.src_rect[3];
    }
    uint32_t dx = 0, dy = 0;
    if (cmd.has_dest_pt) {
      dx = cmd.dest_pt[0];
      dy = cmd.dest_pt[1];
    }
    x1 = Sc(x1), y1 = Sc(y1), x2 = Sc(x2), y2 = Sc(y2), dx = Sc(dx), dy = Sc(dy);
    const uint32_t tw = Sc(f.width), th = Sc(f.height);
    ResolvedTex& rt = g_resolve_targets[f.base];
    if (!rt.tex || rt.w != tw || rt.h != th || rt.family != src->copy_format) {
      SafeRelease(rt.srv);
      SafeRelease(rt.tex);
      D3D11_TEXTURE2D_DESC td{};
      td.Width = tw;
      td.Height = th;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = src->copy_format;
      td.SampleDesc.Count = 1;
      td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      native::Device()->CreateTexture2D(&td, nullptr, &rt.tex);
      D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
      sd.Format = src->view_format;
      sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
      sd.Texture2D.MipLevels = 1;
      native::Device()->CreateShaderResourceView(rt.tex, &sd, &rt.srv);
      rt.w = tw;
      rt.h = th;
      rt.family = src->copy_format;
      Log("resolve: new target %08X %ux%u fmt %u from %s", f.base, f.width, f.height, f.format, src->depth ? "depth" : "color");
    }
    x2 = std::min(x2, src->w);
    y2 = std::min(y2, src->h);
    if (x2 > x1 && y2 > y1 && dx < rt.w && dy < rt.h) {
      const uint32_t cw = std::min(x2 - x1, rt.w - dx), ch = std::min(y2 - y1, rt.h - dy);
      D3D11_BOX box{x1, y1, 0, x1 + cw, y1 + ch, 1};
      ctx->CopySubresourceRegion(rt.tex, 0, dx, dy, 0, src->tex, 0, &box);
    }
    tex::SetResolved(f.base, rt.w, rt.h, rt.tex, rt.srv);
  }
  // optional clears after the copy
  if ((flags & 0x100) && !src->depth && src->rtv) {
    float col[4] = {0, 0, 0, 0};
    if (cmd.has_clear_color)
      for (int i = 0; i < 4; ++i) col[i] = cmd.clear_color[i];
    ctx->ClearRenderTargetView(src->rtv, col);
  }
  if ((flags & 0x200) && bt.depth) ctx->ClearDepthStencilView(bt.depth->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, float(c.f1.f64), 0);
}

// ---------------------------------------------------------------------------------------------
// Present
ID3D11VertexShader* g_blit_vs = nullptr;
ID3D11PixelShader* g_blit_ps = nullptr;
ID3D11SamplerState* g_blit_smp = nullptr;
const char kBlit[] = R"(
Texture2D<float4> t : register(t0);
SamplerState s : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs_main(uint id : SV_VertexID) { V o; o.uv = float2((id << 1) & 2, id & 2); o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
float4 ps_main(V i) : SV_Target { return float4(t.Sample(s, i.uv).rgb, 1.0); }

// FXAA (after Lottes' FXAA 3.11 quality algorithm), in source-image pixels.
// k.xy = 1 / source size, p.x = subpixel amount, p.y = edge threshold, p.z = minimum threshold.
cbuffer FxaaCB : register(b0) { float4 k; float4 p; };
float Lum(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float LumAt(float2 uv) { return Lum(t.SampleLevel(s, uv, 0).rgb); }
float4 ps_fxaa(V i) : SV_Target {
  const float2 px = k.xy, uv = i.uv;
  const float3 rgbM = t.SampleLevel(s, uv, 0).rgb;
  const float lM = Lum(rgbM);
  const float lN = LumAt(uv + float2(0, -px.y)), lS = LumAt(uv + float2(0, px.y));
  const float lW = LumAt(uv + float2(-px.x, 0)), lE = LumAt(uv + float2(px.x, 0));
  const float lMax = max(max(max(lN, lS), max(lW, lE)), lM), lMin = min(min(min(lN, lS), min(lW, lE)), lM);
  const float range = lMax - lMin;
  if (range < max(p.z, lMax * p.y)) return float4(rgbM, 1);
  const float lNW = LumAt(uv - px), lSE = LumAt(uv + px);
  const float lNE = LumAt(uv + float2(px.x, -px.y)), lSW = LumAt(uv + float2(-px.x, px.y));
  const float lNS = lN + lS, lWE = lW + lE;
  const float edgeH = abs(lNW + lSW - 2 * lW) + 2 * abs(lNS - 2 * lM) + abs(lNE + lSE - 2 * lE);
  const float edgeV = abs(lNW + lNE - 2 * lN) + 2 * abs(lWE - 2 * lM) + abs(lSW + lSE - 2 * lS);
  const bool horz = edgeH >= edgeV;
  const float l1 = horz ? lN : lW, l2 = horz ? lS : lE;
  const float g1 = l1 - lM, g2 = l2 - lM;
  const bool steep1 = abs(g1) >= abs(g2);
  const float gScaled = 0.25 * max(abs(g1), abs(g2));
  float stepLen = horz ? px.y : px.x;
  float lLocal;
  if (steep1) { stepLen = -stepLen; lLocal = 0.5 * (l1 + lM); } else { lLocal = 0.5 * (l2 + lM); }
  float2 cuv = uv;
  if (horz) cuv.y += 0.5 * stepLen; else cuv.x += 0.5 * stepLen;
  const float2 off = horz ? float2(px.x, 0) : float2(0, px.y);
  float2 uv1 = cuv - off, uv2 = cuv + off;
  float e1 = LumAt(uv1) - lLocal, e2 = LumAt(uv2) - lLocal;
  bool r1 = abs(e1) >= gScaled, r2 = abs(e2) >= gScaled;
  if (!r1) uv1 -= off;
  if (!r2) uv2 += off;
  const float Q[12] = {1, 1, 1, 1, 1, 1.5, 2, 2, 2, 2, 4, 8};
  [loop] for (int n = 1; n < 12 && !(r1 && r2); ++n) {
    if (!r1) { e1 = LumAt(uv1) - lLocal; r1 = abs(e1) >= gScaled; if (!r1) uv1 -= off * Q[n]; }
    if (!r2) { e2 = LumAt(uv2) - lLocal; r2 = abs(e2) >= gScaled; if (!r2) uv2 += off * Q[n]; }
  }
  const float d1 = horz ? uv.x - uv1.x : uv.y - uv1.y, d2 = horz ? uv2.x - uv.x : uv2.y - uv.y;
  const bool closer1 = d1 < d2;
  const float len = max(d1 + d2, 1e-6);
  const float pixOff = -min(d1, d2) / len + 0.5;
  const bool mSmaller = lM < lLocal;
  const bool good = ((closer1 ? e1 : e2) < 0) != mSmaller;
  float fin = good ? pixOff : 0;
  const float lAvg = (2 * (lNS + lWE) + (lNW + lNE + lSW + lSE)) / 12.0;
  const float sub1 = saturate(abs(lAvg - lM) / range);
  const float sub2 = (-2 * sub1 + 3) * sub1 * sub1;
  fin = max(fin, sub2 * sub2 * p.x);
  float2 fuv = uv;
  if (horz) fuv.y += fin * stepLen; else fuv.x += fin * stepLen;
  return float4(t.SampleLevel(s, fuv, 0).rgb, 1);
}
)";
ID3D11PixelShader* g_fxaa_ps = nullptr;
ID3D11Buffer* g_fxaa_cb = nullptr;
bool g_fxaa_failed = false;

// 0 = off, 1 = FXAA, 2 = FXAA extreme (swap_post_effect, as in the SDK's GPU). Read every 30
// frames: the cvar is a string set by the menus on other threads.
int g_post_mode = 0;
uint64_t g_post_checked = ~0ull;
int PostEffect(uint64_t frame) {
  if (g_post_checked == ~0ull || frame - g_post_checked >= 30) {
    g_post_checked = frame;
    const std::string v = rex::cvar::GetFlagByName("swap_post_effect");
    g_post_mode = v == "fxaa" ? 1 : v == "fxaa_extreme" ? 2 : 0;
    if (const char* e = std::getenv("Q4_HLE_FXAA"); e && *e) g_post_mode = std::clamp(std::atoi(e), 0, 2);
  }
  return g_post_mode;
}

// Draws srv into the back buffer, letterboxed (or into `target` with viewport `tvp`), optionally
// through FXAA.
void Blit(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, int post,
          ID3D11RenderTargetView* target = nullptr, const D3D11_VIEWPORT* tvp = nullptr) {
  auto* ctx = native::Context();
  if (!g_blit_vs) {
    ID3DBlob *b = nullptr, *e = nullptr;
    if (SUCCEEDED(D3DCompile(kBlit, sizeof(kBlit) - 1, nullptr, nullptr, nullptr, "vs_main", "vs_5_0", 0, 0, &b, &e))) {
      native::Device()->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_blit_vs);
      b->Release();
    }
    SafeRelease(e);
    if (SUCCEEDED(D3DCompile(kBlit, sizeof(kBlit) - 1, nullptr, nullptr, nullptr, "ps_main", "ps_5_0", 0, 0, &b, &e))) {
      native::Device()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_blit_ps);
      b->Release();
    }
    SafeRelease(e);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    native::Device()->CreateSamplerState(&sd, &g_blit_smp);
  }
  if (post && !g_fxaa_ps && !g_fxaa_failed) {
    ID3DBlob *b = nullptr, *e = nullptr;
    if (SUCCEEDED(D3DCompile(kBlit, sizeof(kBlit) - 1, nullptr, nullptr, nullptr, "ps_fxaa", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) {
      native::Device()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_fxaa_ps);
      b->Release();
      g_fxaa_cb = MakeCB(32);
      Log("blit: FXAA shader ready");
    } else {
      Log("blit: FXAA shader failed: %s", e ? static_cast<const char*>(e->GetBufferPointer()) : "?");
      g_fxaa_failed = true;
    }
    SafeRelease(e);
  }
  const bool fxaa = post && g_fxaa_ps && g_fxaa_cb;
  if (fxaa) {
    const float cb[8] = {1.0f / float(w), 1.0f / float(h), 0, 0,
                         post == 2 ? 1.0f : 0.75f, post == 2 ? 0.063f : 0.166f, post == 2 ? 0.0312f : 0.0833f, 0};
    UpdateCB(g_fxaa_cb, cb, sizeof(cb));
    ctx->PSSetConstantBuffers(0, 1, &g_fxaa_cb);
  }
  ID3D11RenderTargetView* bb = target ? target : native::BackBufferRTV();
  ctx->OMSetRenderTargets(1, &bb, nullptr);
  const float bw = float(native::BackBufferWidth()), bh = float(native::BackBufferHeight());
  // letterbox to the source aspect ratio
  float vw = bw, vh = bw * float(h) / float(w);
  if (vh > bh) vh = bh, vw = bh * float(w) / float(h);
  D3D11_VIEWPORT vp{(bw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0, 1};
  if (tvp) vp = *tvp;
  ctx->RSSetViewports(1, &vp);
  ctx->RSSetState(nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(g_blit_vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->PSSetShader(fxaa ? g_fxaa_ps : g_blit_ps, nullptr, 0);
  ctx->PSSetShaderResources(0, 1, &srv);
  ctx->PSSetSamplers(0, 1, &g_blit_smp);
  ctx->Draw(3, 0);
  ID3D11ShaderResourceView* nul = nullptr;
  ctx->PSSetShaderResources(0, 1, &nul);
  InvalidateBindings();
}

// ---------------------------------------------------------------------------------------------
// Stereoscopic 3D (side by side), from the depth buffer.
//
// The scene is rendered once. Its colour and depth are copied when the frame switches from 3D
// drawing (depth test on) to 2D overlays (depth test off) on the full-screen target. At present
// time each eye's image is built by shifting scene pixels sideways by a parallax computed from
// depth (far = behind the screen, near = in front), searching for the source pixel that lands on
// each output pixel and keeping the nearest when several do. Pixels that differ between the
// final image and the scene copy are overlays (HUD, menus, subtitles) and are kept at screen
// depth. q4hle_stereo: 0 off, 1 half side-by-side (each eye squeezed to half width, for TVs and
// glasses), 2 full side-by-side (each eye at full aspect in one half). q4hle_stereo_depth 1-3.
// Q4_HLE_STEREO=0/1/2 overrides.
struct SnapTex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  UINT w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
SnapTex g_snap_color, g_snap_depth;
ID3D11PixelShader* g_stereo_ps = nullptr;
ID3D11Buffer* g_stereo_cb = nullptr;
bool g_stereo_failed = false;

void RefreshStereoSettings() {
  auto get = [](const char* n, int def) {
    const std::string v = rex::cvar::GetFlagByName(n);
    return v.empty() ? def : std::atoi(v.c_str());
  };
  g_stereo_mode = std::clamp(get("q4hle_stereo", 0), 0, 2);
  g_stereo_depth = std::clamp(get("q4hle_stereo_depth", 2), 1, 3);
  if (const char* e = std::getenv("Q4_HLE_STEREO"); e && *e) g_stereo_mode = std::clamp(std::atoi(e), 0, 2);
  if (g_crt_preset) g_stereo_mode = 0;  // the CRT filter takes the whole picture
}

bool CopyToSnap(SnapTex& st, RenderTarget* rt) {
  if (!rt || !rt->tex) return false;
  D3D11_TEXTURE2D_DESC td;
  rt->tex->GetDesc(&td);
  if (!st.tex || st.w != td.Width || st.h != td.Height || st.fmt != rt->view_format) {
    SafeRelease(st.srv);
    SafeRelease(st.tex);
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;
    td.CPUAccessFlags = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    if (FAILED(native::Device()->CreateTexture2D(&td, nullptr, &st.tex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = rt->view_format;
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    if (FAILED(native::Device()->CreateShaderResourceView(st.tex, &sd, &st.srv))) {
      SafeRelease(st.tex);
      return false;
    }
    st.w = td.Width;
    st.h = td.Height;
    st.fmt = rt->view_format;
  }
  native::Context()->CopyResource(st.tex, rt->tex);
  return true;
}

void StereoSnapshot() {
  g_st_pending = false;
  g_st_have = CopyToSnap(g_snap_color, g_st_rt) && CopyToSnap(g_snap_depth, g_st_depth_rt);
}

// Render thread, before each draw: notice the switch from the 3D scene to 2D overlays.
void StereoTrackDraw(const Regs& R, const BoundTargets& bt) {
  if (!g_stereo_mode) return;
  const bool ztest = (R(RB_DEPTHCONTROL) >> 1) & 1;
  RenderTarget* c0 = bt.color[0];
  if (!c0 || c0->w < Sc(1280u)) return;
  if (ztest && bt.depth) {
    if (c0 != g_st_rt) g_st_have = false;
    g_st_rt = c0;
    g_st_depth_rt = bt.depth;
    g_st_pending = true;
  } else if (!ztest && g_st_pending && c0 == g_st_rt) {
    StereoSnapshot();
  }
}

const char kStereo[] = R"(
Texture2D<float4> tFinal : register(t0);
Texture2D<float4> tScene : register(t1);
Texture2D<float> tDepth : register(t2);
SamplerState s : register(s0);
// k: x,y = source size in pixels, z = eye (-1 left, +1 right), w = max parallax in pixels
// q: x = depth (1/z) at the screen plane, y = 1 if depth is reversed, z = scene copy present
// r: x,y = size of the scene copy (the whole render target)
cbuffer StereoCB : register(b0) { float4 k; float4 q; float4 r; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float InvZ(float d) { return q.y > 0.5 ? d : 1.0 - d; }
// on-screen parallax (pixels between the eyes' images) of a point: + behind the screen, - in front
float Parallax(float inv_z) { return clamp(k.w * (1.0 - inv_z / q.x), -0.35 * k.w, k.w); }
float4 ps_stereo(V i) : SV_Target {
  const float2 sz = k.xy;
  const float2 pos = i.uv * sz;
  const int2 ip = int2(min(pos, sz - 1));
  const float4 fin = tFinal.Load(int3(ip, 0));
  if (q.z < 0.5) return float4(fin.rgb, 1);
  const float4 scn = tScene.Load(int3(ip, 0));
  if (any(abs(fin.rgb - scn.rgb) > 2.5 / 255.0)) return float4(fin.rgb, 1);  // overlay: screen depth
  // find a source pixel x whose image in this eye lands here: x + eye * P(x) / 2 == pos.x
  const int N = 40;
  const float range = 0.5 * k.w * 1.4;
  float best_x = -1.0, best_near = -1e9;
  [loop] for (int n = 0; n < N; ++n) {
    const float off = lerp(-range, range, (n + 0.5) / N);
    const float sx = pos.x - off;
    if (sx < 0 || sx >= sz.x) continue;
    const float iz = InvZ(tDepth.Load(int3(int(sx), ip.y, 0)));
    const float shift = k.z * 0.5 * Parallax(iz);
    if (abs(shift - off) <= range / N + 0.5 && iz > best_near) {
      best_near = iz;
      best_x = sx;
    }
  }
  if (best_x < 0) {
    // disocclusion: fill from the far side (background)
    const float iz = InvZ(tDepth.Load(int3(ip, 0)));
    best_x = clamp(pos.x - k.z * 0.5 * Parallax(min(iz, q.x * 0.25)), 0, sz.x - 1);
  }
  return float4(tScene.SampleLevel(s, float2(best_x + 0.5, pos.y) / r.xy, 0).rgb, 1);
}
)";
// Draws the two eyes side by side into the back buffer. Returns false if the shader is missing.
bool StereoBlit(ID3D11ShaderResourceView* final_srv, uint32_t w, uint32_t h) {
  auto* ctx = native::Context();
  if (!g_blit_vs) return false;  // Blit() creates the shared vertex shader and sampler
  if (!g_stereo_ps && !g_stereo_failed) {
    ID3DBlob *b = nullptr, *e = nullptr;
    if (SUCCEEDED(D3DCompile(kStereo, sizeof(kStereo) - 1, nullptr, nullptr, nullptr, "ps_stereo", "ps_5_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) {
      native::Device()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_stereo_ps);
      b->Release();
      g_stereo_cb = MakeCB(48);
      Log("stereo: shader ready");
    } else {
      Log("stereo: shader failed: %s", e ? static_cast<const char*>(e->GetBufferPointer()) : "?");
      g_stereo_failed = true;
    }
    SafeRelease(e);
  }
  if (!g_stereo_ps || !g_stereo_cb) return false;
  ID3D11RenderTargetView* bb = native::BackBufferRTV();
  ctx->OMSetRenderTargets(1, &bb, nullptr);
  const float bw = float(native::BackBufferWidth()), bh = float(native::BackBufferHeight());
  D3D11_VIEWPORT eye[2];
  if (g_stereo_mode == 1) {  // half side by side: the normal picture, each eye squeezed to half width
    float vw = bw, vh = bw * float(h) / float(w);
    if (vh > bh) vh = bh, vw = bh * float(w) / float(h);
    const float x0 = (bw - vw) * 0.5f, y0 = (bh - vh) * 0.5f;
    eye[0] = {x0 * 0.5f, y0, vw * 0.5f, vh, 0, 1};
    eye[1] = {bw * 0.5f + x0 * 0.5f, y0, vw * 0.5f, vh, 0, 1};
  } else {  // full side by side: each eye at its own aspect ratio in one half
    const float hw = bw * 0.5f;
    float vw = hw, vh = hw * float(h) / float(w);
    if (vh > bh) vh = bh, vw = bh * float(w) / float(h);
    eye[0] = {(hw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0, 1};
    eye[1] = {hw + (hw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0, 1};
  }
  static const float kStrength[4] = {0.0f, 0.005f, 0.008f, 0.012f};  // low / medium / high
  const float max_parallax = kStrength[g_stereo_depth] * float(w);
  const float conv = 0.03f;  // 1/z at the screen plane (assumes near plane 3: about 100 units)
  ctx->RSSetState(nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(g_blit_vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ctx->PSSetShader(g_stereo_ps, nullptr, 0);
  ID3D11ShaderResourceView* srvs[3] = {final_srv, g_st_have ? g_snap_color.srv : nullptr,
                                       g_st_have ? g_snap_depth.srv : nullptr};
  ctx->PSSetShaderResources(0, 3, srvs);
  ctx->PSSetSamplers(0, 1, &g_blit_smp);
  for (int e = 0; e < 2; ++e) {
    const float cb[12] = {float(w), float(h), e == 0 ? -1.0f : 1.0f, max_parallax,
                          conv, g_depth_clear < 0.5f ? 1.0f : 0.0f, g_st_have ? 1.0f : 0.0f, 0,
                          float(g_snap_color.w ? g_snap_color.w : w), float(g_snap_color.h ? g_snap_color.h : h), 0, 0};
    UpdateCB(g_stereo_cb, cb, sizeof(cb));
    ctx->PSSetConstantBuffers(0, 1, &g_stereo_cb);
    ctx->RSSetViewports(1, &eye[e]);
    ctx->Draw(3, 0);
  }
  ID3D11ShaderResourceView* nul[3] = {};
  ctx->PSSetShaderResources(0, 3, nul);
  InvalidateBindings();
  return true;
}

// ---------------------------------------------------------------------------------------------
// CRT filter: Real CRT Phosphor v1.1 by gunkun (concept, direction, testing, tuning; code largely
// AI-written), https://github.com/crt2oldgamer-stack/real-crt-phosphor. Free for non-commercial
// use with credit; its terms are in docs/THIRD_PARTY_NOTICES.md. Ported from the RetroArch slang
// chain to D3D11, with the "4K PS2" preset values (grille pitch 4.5 px; slot mask pitch 3 px).
//
// The picture is emitted only by red, green and blue phosphor stripes: the frame is first reduced
// to a CRT-like line count (q4hle: 240, 360 or 480 lines, chosen in the menu, instead of the
// original's guess from the source height), run through an analog signal stage (luma/chroma
// smear, peaking, saturation, levels, S-curve), drawn as gaussian scanline beams whose width
// follows brightness, then split into R/G/B stripes (aperture grille or slot mask) that widen
// with brightness and melt into glow in highlights, and finally softened. Passes:
//   box (frame -> line grid), signal, vblur, glow_h, glow_v  at the line grid
//   beam, rgb, post_h  at the output size; post_v writes the back buffer.
// Dither (0) and soft (0) are off in the preset and skipped; the kern pass's weights are computed
// in the signal pass. q4hle_crt_filter: 0 off; 1-3 grille 240/360/480; 4-6 slot 240/360/480.
// Q4_HLE_CRT=0-6 overrides. FXAA, if on, is applied first; 3D mode is off while it is on.
const char kCrt[] = R"(
#define LUMA_BLUR 0.15
#define LUMA_TAIL 0.25
#define PEAKING 0.10
#define CHROMA_BLUR 0.30
#define CHROMA_TAIL 0.60
#define SATURATION 1.30
#define HUE 0.0
#define LV_BLACK 0.0
#define LV_WHITE 0.96
#define S_LOW 0.55
#define S_HIGH 0.30
#define GAMMA_IN 1.0
#define WP float3(0.79, 0.83, 0.91)
#define VERT_BLUR 0.35
#define BEAM_DARK 0.18
#define BEAM_BRIGHT 0.32
#define GLOW 0.12
#define GLOW_R 0.8
#define ST_DARK 0.45
#define ST_BRIGHT 2.2
#define ST_EDGE 0.7
#define MELT_START 0.3
#define MELT_END 0.7
#define MELT_DARK 0.03
#define MELT_BRIGHT 0.6
#define MELT_BLOB 0.38
#define RGB_BLUR_X 0.6
#define RGB_BLUR_Y 0.45
#define KNEE 0.8
#define FINAL_BLUR 0.4
#define BRIGHT 1.0
#define SLOT_LINES 2.0
#define SLOT_VADJ 1.0
#define SLOT_SHAPE 1.0
#define SLOT_GAP 0.22
#define SLOT_DEPTH 0.6
#define SLOT_EDGE 0.35
#define SLOT_MELT 0.65

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
SamplerState sp : register(s0);  // point, clamp
SamplerState sl : register(s1);  // linear, clamp
// s0: input size (xy) and 1/size (zw); o: this pass's output size and 1/size;
// m: x = triad pitch px, y = mask type (0 grille, 1 slot), z = lines, w = 0
cbuffer CrtCB : register(b0) { float4 s0; float4 o; float4 m; };
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

float gmod(float x, float y) { return x - y * floor(x / y); }
float3 ToLinear(float3 c) { return pow(saturate(c), 2.2 * GAMMA_IN) * WP; }
float3 ToYIQ(float3 c) {
  return float3(dot(c, float3(0.299, 0.587, 0.114)), dot(c, float3(0.596, -0.274, -0.322)),
                dot(c, float3(0.211, -0.523, 0.312)));
}
float3 FromYIQ(float3 c) {
  return float3(c.x + 0.956 * c.y + 0.621 * c.z, c.x - 0.272 * c.y - 0.647 * c.z, c.x - 1.106 * c.y + 1.703 * c.z);
}
float Erfc(float x) {
  float z = abs(x);
  float t = 1.0 / (1.0 + 0.3275911 * z);
  float y = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429)))) * exp(-z * z);
  return (x >= 0.0) ? y : 2.0 - y;
}
float ExGauss(float t, float s, float tau) {
  s = max(s, 0.05);
  if (tau < 0.05) return exp(-0.5 * t * t / (s * s));
  float l = 1.0 / tau;
  float e = clamp(l * (0.5 * l * s * s - t), -80.0, 80.0);
  return exp(e) * Erfc((l * s * s - t) / (1.41421356 * s));
}
float NormalInterval(float a, float b) {
  if (a >= 0.0) return 0.5 * (Erfc(a) - Erfc(b));
  if (b <= 0.0) return 0.5 * (Erfc(-b) - Erfc(-a));
  return 1.0 - 0.5 * (Erfc(-a) + Erfc(b));
}
float StripePrimitive(float x, float w, float e) {
  float h = 0.5 * w;
  float inner = h * (1.0 - e);
  float r = min(abs(x), h);
  float t = saturate((h - r) / max(h * e, 1e-8));
  float area = (r <= inner) ? r : inner + h * e * (0.5 - t * t * t + 0.5 * t * t * t * t);
  return sign(x) * area;
}
float K(float nW) { return clamp(nW / 256.0, 0.5, 1.25); }
float4 At(int2 p) { return t0.Load(int3(clamp(p, int2(0, 0), int2(s0.xy) - 1), 0)); }

// pass 0: average the frame down to the line grid (approximately linear light, like the original)
float4 ps_box(V i) : SV_Target {
  const float2 cell = s0.xy * o.zw;  // source pixels per grid pixel
  const float2 base = floor(i.pos.xy) * cell;
  float3 acc = 0;
  [unroll] for (int y = 0; y < 4; ++y)
    [unroll] for (int x = 0; x < 4; ++x) {
      float3 c = t0.SampleLevel(sl, (base + (float2(x, y) + 0.5) * cell * 0.25) * s0.zw, 0).rgb;
      acc += c * c;
    }
  return float4(sqrt(acc / 16.0), 1);
}

// pass 4: analog signal path
float4 ps_signal(V i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float k = K(s0.x);
  const float sY = LUMA_BLUR * k, tY = LUMA_TAIL * k, sY2 = sqrt(sY * sY + 1.44 * k * k);
  const float sC = CHROMA_BLUR * k, tC = CHROMA_TAIL * k;
  const float sMax = max(max(sY2, sC), 0.5);
  const int lo = max(-int(ceil(4.0 * sMax)), -32), hi = min(int(ceil(4.0 * sMax + 6.0 * max(tY, tC))), 64);
  float Y1 = 0, W1 = 0, Y2 = 0, W2 = 0, W3 = 0;
  float2 C = 0;
  [loop] for (int j = lo; j <= hi; ++j) {
    const float t = float(j);
    const float3 yiq = ToYIQ(At(p - int2(j, 0)).rgb);
    const float w1 = ExGauss(t, sY, tY), w2 = ExGauss(t, sY2, tY), w3 = ExGauss(t, sC, tC);
    Y1 += w1 * yiq.x; W1 += w1;
    Y2 += w2 * yiq.x; W2 += w2;
    C += w3 * yiq.yz; W3 += w3;
  }
  const float yl = Y1 / max(W1, 1e-6);
  const float Y = yl + PEAKING * (yl - Y2 / max(W2, 1e-6));
  float2 c = C / max(W3, 1e-6);
  const float h = radians(HUE);
  c = float2(c.x * cos(h) - c.y * sin(h), c.x * sin(h) + c.y * cos(h)) * SATURATION;
  float3 rgb = saturate(FromYIQ(float3(Y, c)));
  const float ungraded = max(rgb.r, max(rgb.g, rgb.b));
  rgb = saturate((rgb - LV_BLACK) / max(LV_WHITE - LV_BLACK, 0.01));
  const float3 sm = rgb * rgb * (3.0 - 2.0 * rgb);
  const float3 kk = lerp(float3(S_HIGH, S_HIGH, S_HIGH), float3(S_LOW, S_LOW, S_LOW), step(rgb, float3(0.5, 0.5, 0.5)));
  rgb = rgb + kk * (sm - rgb);
  return float4(rgb, ungraded);
}

// pass 5: vertical softness between neighbouring lines
float4 ps_vblur(V i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float vb = VERT_BLUR, ge = 2.2 * GAMMA_IN;
  const int r = min(int(ceil(3.0 * vb)), 6);
  float4 acc = 0;
  float ws = 0;
  [loop] for (int j = -r; j <= r; ++j) {
    const float w = exp(-0.5 * j * j / (vb * vb));
    acc += w * pow(saturate(At(p + int2(0, j))), ge);
    ws += w;
  }
  return pow(acc / ws, 1.0 / ge);
}

// passes 6 and 7: glow (linear light)
float4 ps_glow_h(V i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float s = max(GLOW_R * 0.875 * K(s0.x), 0.1);
  const int r = min(int(ceil(3.0 * s)), 24);
  float3 acc = 0;
  float ws = 0;
  [loop] for (int j = -r; j <= r; ++j) {
    const float w = exp(-0.5 * j * j / (s * s));
    acc += w * ToLinear(At(p + int2(j, 0)).rgb);
    ws += w;
  }
  return float4(acc / ws, 1);
}
float4 ps_glow_v(V i) : SV_Target {
  const int2 p = int2(i.pos.xy);
  const float s = max(GLOW_R * 0.6, 0.1);
  const int r = min(int(ceil(3.0 * s)), 16);
  float3 acc = 0;
  float ws = 0;
  [loop] for (int j = -r; j <= r; ++j) {
    const float w = exp(-0.5 * j * j / (s * s));
    acc += w * At(p + int2(0, j)).rgb;
    ws += w;
  }
  return float4(acc / ws, 1);
}

// pass 8: scanline beams + glow, at the output size. t0 = glow, t1 = signal (s0 = signal size)
float4 ps_beam(V i) : SV_Target {
  const float2 uv = i.uv;
  const float lines = s0.y, nW = s0.x;
  const float ny = uv.y * lines;
  const float nx = uv.x * nW - 0.5;
  const float x0 = floor(nx), fx = nx - x0;
  const float base = floor(ny - 0.5);
  const float gexp = 2.2 * GAMMA_IN;
  float3 acc = 0;
  [unroll] for (int k = -1; k <= 2; ++k) {
    const float li = clamp(base + k, 0.0, lines - 1.0);
    const float d = abs(ny - (base + k + 0.5));
    const float4 sA = t1.Load(int3(int(clamp(x0, 0.0, nW - 1.0)), int(li), 0));
    const float4 sB = t1.Load(int3(int(clamp(x0 + 1.0, 0.0, nW - 1.0)), int(li), 0));
    const float3 col = lerp(ToLinear(sA.rgb), ToLinear(sB.rgb), fx);
    const float lu = lerp(pow(saturate(sA.a), gexp), pow(saturate(sB.a), gexp), fx);
    const float w = BEAM_DARK + (BEAM_BRIGHT - BEAM_DARK) * sqrt(saturate(lu));
    acc += col * exp(-0.5 * (d / w) * (d / w)) / (w * 2.5066);
  }
  acc += GLOW * t0.SampleLevel(sl, uv, 0).rgb;
  return float4(acc, 1);
}

// pass 9: light emitted only by R, G, B phosphor stripes. t0 = beam image (output size)
float SlotEmission(float py, float phase, float P) {
  const float gap = clamp(SLOT_GAP, 0.0, 0.6);
  if (gap <= 0.0) return 1.0;
  const float edge = clamp(SLOT_EDGE, 0.05, 1.0);
  const float width = gap * P;
  const float d = gmod(py - phase + 0.5 * P, P) - 0.5 * P;
  float area = 0.0;
  if (P * (1.0 - gap) >= 1.00001) {
    area = StripePrimitive(d + 0.5, width, edge) - StripePrimitive(d - 0.5, width, edge);
  } else {
    [unroll] for (int j = -1; j <= 1; ++j) {
      const float q = d - j * P;
      area += StripePrimitive(q + 0.5, width, edge) - StripePrimitive(q - 0.5, width, edge);
    }
  }
  const float depth = clamp(SLOT_DEPTH, 0.0, 1.0);
  return (1.0 - depth * saturate(area)) / (1.0 - depth * gap * (1.0 - 0.5 * edge));
}
float4 ps_rgb(V i) : SV_Target {
  const float2 uv = i.uv;
  const float T = m.x;
  const float px = uv.x * o.x;
  const float y = uv.y;
  const float e = clamp(ST_EDGE, 0.05, 1.0);
  const float tri = floor(px / T);
  float3 outc = 0;
  const bool slot = m.y > 0.5;
  float2 gates = 1;
  float yW = y;
  if (slot) {
    const float py = uv.y * o.y;
    const float linePx = o.y / max(m.z, 1.0);
    const float nl = floor(clamp(SLOT_LINES, 2.0, 4.0) + 0.5);
    const float va = clamp(SLOT_VADJ, 0.5, 2.0);
    const float P = max(2.0, linePx * nl * va);
    const float ph = floor(nl * 0.5 + 0.5) * linePx * va;
    if (SLOT_SHAPE > 0.5) yW = (floor(py / linePx) + 0.5) * linePx * o.w;
    gates = float2(SlotEmission(py, 0.0, P), SlotEmission(py, ph, P));
  }
  const float reach = 0.5 * (T / 3.0) * max(ST_BRIGHT, ST_DARK) + 0.5;
  [unroll] for (int c = 0; c < 3; ++c) {
    const float off = (c + 0.5) / 3.0;
    const float n0 = max(floor((px - reach) / T - off) + 1.0, tri - 3.0);
    const float n1 = min(ceil((px + reach) / T - off) - 1.0, tri + 3.0);
    [loop] for (int k = 0; k < 4; ++k) {
      const float n = n0 + k;
      if (n > n1) break;
      const float xc = (n + off) * T;
      const float V_ = max(t0.SampleLevel(sl, float2(xc * o.z, y), 0)[c], 0.0);
      float Vw = V_;
      if (yW != y) Vw = max(t0.SampleLevel(sl, float2(xc * o.z, yW), 0)[c], 0.0);
      const float s = ST_DARK + (ST_BRIGHT - ST_DARK) * sqrt(saturate(Vw));
      const float w = (T / 3.0) * s;
      const float d = px - xc;
      const float prof = StripePrimitive(d + 0.5, w, e) - StripePrimitive(d - 0.5, w, e);
      const float gain = T / (w * (1.0 - 0.5 * e));
      const float gate = slot ? (gmod(n, 2.0) < 1.0 ? gates.x : gates.y) : 1.0;
      outc[c] += V_ * prof * gain * gate;
    }
  }
  const float sig = max(MELT_BLOB, 0.05) * T;
  float3 blobs = 0;
  const int rb = int(ceil((4.0 * sig + 0.5) / T)) + 1;
  [loop] for (int di = -5; di <= 5; ++di) {
    if (abs(di) > rb) continue;
    const float tc = (tri + di + 0.5) * T;
    const float3 C = max(t0.SampleLevel(sl, float2(tc * o.z, y), 0).rgb, 0.0);
    const float g = T * NormalInterval((px - 0.5 - tc) / (1.41421356 * sig), (px + 0.5 - tc) / (1.41421356 * sig));
    float gate = 1.0;
    if (slot) gate = lerp(gmod(tri + di, 2.0) < 1.0 ? gates.x : gates.y, 1.0, saturate(SLOT_MELT));
    blobs += C * g * gate;
  }
  const float3 h0 = t0.SampleLevel(sl, uv, 0).rgb;
  const float L = max(h0.r, max(h0.g, h0.b));
  float kk = saturate((L - MELT_START) / max(MELT_END - MELT_START, 0.01));
  kk = kk * kk * (3.0 - 2.0 * kk);
  const float dif = MELT_DARK + (MELT_BRIGHT - MELT_DARK) * kk;
  return float4(lerp(outc, blobs, dif), 1);
}

// passes 10 and 11: blur after the RGB split, then brightness, highlight knee and gamma
float3 Blur(float2 uv, float2 stepUV, float s) {
  float3 acc = t0.SampleLevel(sl, uv, 0).rgb;
  float ws = 1.0;
  if (s > 0.05) {
    const int r = min(int(ceil(3.0 * s)), 24);
    [loop] for (int j = 1; j <= r; ++j) {
      const float w = exp(-0.5 * j * j / (s * s));
      acc += w * (t0.SampleLevel(sl, uv + j * stepUV, 0).rgb + t0.SampleLevel(sl, uv - j * stepUV, 0).rgb);
      ws += 2.0 * w;
    }
  }
  return acc / ws;
}
float BlurSigma(float b) { const float s0_ = b * m.x / 6.0, sf = FINAL_BLUR * m.x / 6.0; return sqrt(s0_ * s0_ + sf * sf); }
float4 ps_post_h(V i) : SV_Target { return float4(Blur(i.uv, float2(o.z, 0), BlurSigma(RGB_BLUR_X)), 1); }
float4 ps_post_v(V i) : SV_Target {
  float3 acc = Blur(i.uv, float2(0, o.w), BlurSigma(RGB_BLUR_Y)) * BRIGHT;
  const float kn = clamp(KNEE, 0.5, 0.999);
  const float3 over = max(acc - kn, 0.0);
  acc = min(acc, kn) + (1.0 - kn) * (1.0 - exp(-over / (1.0 - kn)));
  return float4(pow(saturate(acc), 1.0 / 2.2), 1);
}
)";

enum CrtPass { kCrtBox, kCrtSignal, kCrtVBlur, kCrtGlowH, kCrtGlowV, kCrtBeam, kCrtRgb, kCrtPostH, kCrtPostV, kCrtPasses };
const char* const kCrtEntry[kCrtPasses] = {"ps_box", "ps_signal", "ps_vblur", "ps_glow_h", "ps_glow_v",
                                           "ps_beam", "ps_rgb", "ps_post_h", "ps_post_v"};
ID3D11PixelShader* g_crt_ps[kCrtPasses] = {};
ID3D11Buffer* g_crt_cb = nullptr;
ID3D11SamplerState* g_crt_point = nullptr;
bool g_crt_failed = false;
struct CrtTex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint32_t w = 0, h = 0;
  DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};
// fxaa (frame size), box/signal/vblur (line grid, 8-bit), glow_h/glow (line grid, float),
// beam/rgb/post_h (output size, float)
enum { kTFxaa, kTBox, kTSignal, kTVBlur, kTGlowH, kTGlow, kTBeam, kTRgb, kTPostH, kTCount };
CrtTex g_crt_t[kTCount];

void RefreshCrtSettings() {
  const std::string v = rex::cvar::GetFlagByName("q4hle_crt_filter");
  int preset = v.empty() ? 0 : std::atoi(v.c_str());
  if (const char* e = std::getenv("Q4_HLE_CRT"); e && *e) preset = std::atoi(e);
  preset = std::clamp(preset, 0, 6);
  if (preset != g_crt_preset) Log("crt filter: mode %d", preset);
  g_crt_preset = preset;
}

bool EnsureCrtShaders() {
  if (g_crt_failed) return false;
  if (g_crt_ps[0]) return true;
  for (int p = 0; p < kCrtPasses; ++p) {
    ID3DBlob *b = nullptr, *e = nullptr;
    if (FAILED(D3DCompile(kCrt, sizeof(kCrt) - 1, nullptr, nullptr, nullptr, kCrtEntry[p], "ps_5_0",
                          D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) {
      Log("crt filter: %s failed: %s", kCrtEntry[p], e ? static_cast<const char*>(e->GetBufferPointer()) : "?");
      SafeRelease(e);
      g_crt_failed = true;
      return false;
    }
    native::Device()->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g_crt_ps[p]);
    b->Release();
    SafeRelease(e);
  }
  g_crt_cb = MakeCB(48);
  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.MaxLOD = D3D11_FLOAT32_MAX;
  native::Device()->CreateSamplerState(&sd, &g_crt_point);
  Log("crt filter: shaders ready");
  return g_crt_cb && g_crt_point;
}

bool EnsureCrtTex(CrtTex& t, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
  if (t.srv && t.w == w && t.h == h && t.fmt == fmt) return true;
  SafeRelease(t.srv);
  SafeRelease(t.rtv);
  SafeRelease(t.tex);
  D3D11_TEXTURE2D_DESC td{};
  td.Width = w;
  td.Height = h;
  td.MipLevels = td.ArraySize = 1;
  td.Format = fmt;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(native::Device()->CreateTexture2D(&td, nullptr, &t.tex)) ||
      FAILED(native::Device()->CreateRenderTargetView(t.tex, nullptr, &t.rtv)) ||
      FAILED(native::Device()->CreateShaderResourceView(t.tex, nullptr, &t.srv))) {
    SafeRelease(t.srv);
    SafeRelease(t.rtv);
    SafeRelease(t.tex);
    return false;
  }
  t.w = w;
  t.h = h;
  t.fmt = fmt;
  return true;
}

// One full-screen pass: inputs a/b, output rtv with viewport vp; s0 = size of input a (or b).
void CrtPassDraw(int pass, ID3D11ShaderResourceView* a, ID3D11ShaderResourceView* b, float sw, float sh,
                 ID3D11RenderTargetView* rtv, const D3D11_VIEWPORT& vp, float triad, float slot, float lines) {
  auto* ctx = native::Context();
  const float cb[12] = {sw, sh, 1.0f / sw, 1.0f / sh, vp.Width, vp.Height, 1.0f / vp.Width, 1.0f / vp.Height,
                        triad, slot, lines, 0};
  UpdateCB(g_crt_cb, cb, sizeof(cb));
  ctx->OMSetRenderTargets(1, &rtv, nullptr);
  ctx->RSSetViewports(1, &vp);
  ctx->PSSetShader(g_crt_ps[pass], nullptr, 0);
  ctx->PSSetConstantBuffers(0, 1, &g_crt_cb);
  ID3D11ShaderResourceView* srvs[2] = {a, b};
  ctx->PSSetShaderResources(0, 2, srvs);
  ctx->Draw(3, 0);
  ID3D11ShaderResourceView* nul[2] = {};
  ctx->PSSetShaderResources(0, 2, nul);
}

// Draws the frame through the CRT filter into the back buffer. Returns false if it cannot.
bool CrtBlit(ID3D11ShaderResourceView* srv, uint32_t w, uint32_t h, int post) {
  if (!g_blit_vs || !EnsureCrtShaders()) return false;
  auto* ctx = native::Context();
  if (post && EnsureCrtTex(g_crt_t[kTFxaa], w, h, DXGI_FORMAT_R8G8B8A8_UNORM)) {
    const D3D11_VIEWPORT full{0, 0, float(w), float(h), 0, 1};
    Blit(srv, w, h, post, g_crt_t[kTFxaa].rtv, &full);
    srv = g_crt_t[kTFxaa].srv;
  }
  static const uint32_t kLines[3] = {240, 360, 480};
  const bool slot = g_crt_preset >= 4;
  const uint32_t lines = kLines[(g_crt_preset - 1) % 3];
  const uint32_t nw = std::max<uint32_t>(16, uint32_t(std::lround(double(lines) * w / h)));
  const float bw = float(native::BackBufferWidth()), bh = float(native::BackBufferHeight());
  float vw = bw, vh = bw * float(h) / float(w);
  if (vh > bh) vh = bh, vw = bh * float(w) / float(h);
  const uint32_t ow = std::max<uint32_t>(1, uint32_t(vw)), oh = std::max<uint32_t>(1, uint32_t(vh));
  const DXGI_FORMAT u8 = DXGI_FORMAT_R8G8B8A8_UNORM, f16 = DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (!EnsureCrtTex(g_crt_t[kTBox], nw, lines, u8) || !EnsureCrtTex(g_crt_t[kTSignal], nw, lines, u8) ||
      !EnsureCrtTex(g_crt_t[kTVBlur], nw, lines, u8) || !EnsureCrtTex(g_crt_t[kTGlowH], nw, lines, f16) ||
      !EnsureCrtTex(g_crt_t[kTGlow], nw, lines, f16) || !EnsureCrtTex(g_crt_t[kTBeam], ow, oh, f16) ||
      !EnsureCrtTex(g_crt_t[kTRgb], ow, oh, f16) || !EnsureCrtTex(g_crt_t[kTPostH], ow, oh, f16))
    return false;
  const float T = slot ? 3.0f : 4.5f, S = slot ? 1.0f : 0.0f, L = float(lines);
  ctx->RSSetState(nullptr);
  ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
  ctx->OMSetDepthStencilState(nullptr, 0);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->IASetInputLayout(nullptr);
  ctx->VSSetShader(g_blit_vs, nullptr, 0);
  ctx->GSSetShader(nullptr, nullptr, 0);
  ID3D11SamplerState* smp[2] = {g_crt_point, g_blit_smp};
  ctx->PSSetSamplers(0, 2, smp);
  const D3D11_VIEWPORT gv{0, 0, float(nw), float(lines), 0, 1};
  const D3D11_VIEWPORT ov{0, 0, float(ow), float(oh), 0, 1};
  const float gw = float(nw);
  CrtPassDraw(kCrtBox, srv, nullptr, float(w), float(h), g_crt_t[kTBox].rtv, gv, T, S, L);
  CrtPassDraw(kCrtSignal, g_crt_t[kTBox].srv, nullptr, gw, L, g_crt_t[kTSignal].rtv, gv, T, S, L);
  CrtPassDraw(kCrtVBlur, g_crt_t[kTSignal].srv, nullptr, gw, L, g_crt_t[kTVBlur].rtv, gv, T, S, L);
  CrtPassDraw(kCrtGlowH, g_crt_t[kTVBlur].srv, nullptr, gw, L, g_crt_t[kTGlowH].rtv, gv, T, S, L);
  CrtPassDraw(kCrtGlowV, g_crt_t[kTGlowH].srv, nullptr, gw, L, g_crt_t[kTGlow].rtv, gv, T, S, L);
  CrtPassDraw(kCrtBeam, g_crt_t[kTGlow].srv, g_crt_t[kTVBlur].srv, gw, L, g_crt_t[kTBeam].rtv, ov, T, S, L);
  CrtPassDraw(kCrtRgb, g_crt_t[kTBeam].srv, nullptr, float(ow), float(oh), g_crt_t[kTRgb].rtv, ov, T, S, L);
  CrtPassDraw(kCrtPostH, g_crt_t[kTRgb].srv, nullptr, float(ow), float(oh), g_crt_t[kTPostH].rtv, ov, T, S, L);
  const D3D11_VIEWPORT bv{(bw - vw) * 0.5f, (bh - vh) * 0.5f, float(ow), float(oh), 0, 1};
  CrtPassDraw(kCrtPostV, g_crt_t[kTPostH].srv, nullptr, float(ow), float(oh), native::BackBufferRTV(), bv, T, S, L);
  InvalidateBindings();
  return true;
}

// ---------------------------------------------------------------------------------------------
// Hooks
template <uint32_t Addr>
void CallHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  capture::Call(Addr, ctx, base, false);
  original(ctx, base);
}

void DrawIndexedHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  PPCContext saved = ctx;
  original(ctx, base);
  capture::Call(0x825EE420, saved, base, true);
  if (!native::Enabled()) return;
  DrawArgs a;
  a.prim = saved.r4.u32 & 0x3F;
  a.indexed = true;
  a.start = saved.r6.u32;
  a.count = saved.r7.u32;
  Draw(base, saved.r3.u32, a);
}

template <uint32_t Addr>
void DrawAutoHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  PPCContext saved = ctx;
  original(ctx, base);
  capture::Call(Addr, saved, base, true);
  if (!native::Enabled()) return;
  DrawArgs a;
  a.prim = saved.r4.u32 & 0x3F;
  a.indexed = false;
  a.start = saved.r5.u32;
  a.count = saved.r6.u32;
  Draw(base, saved.r3.u32, a);
}

// BeginVertices(device, prim, vertex_count, stride) returns a pointer the game fills; the draw is
// issued by EndVertices(device).
DrawArgs g_pending_up;
bool g_has_pending_up = false;

void BeginVerticesHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  PPCContext saved = ctx;
  original(ctx, base);
  g_pending_up = {};
  g_pending_up.prim = saved.r4.u32 & 0x3F;
  g_pending_up.count = saved.r5.u32;
  g_pending_up.up_va = ctx.r3.u32;
  g_pending_up.up_bytes = saved.r5.u32 * saved.r6.u32;
  g_has_pending_up = ctx.r3.u32 != 0;
}

void EndVerticesHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  PPCContext saved = ctx;
  original(ctx, base);
  capture::Call(0x825EE1C0, saved, base, true);
  if (!g_has_pending_up || !native::Enabled()) return;
  g_has_pending_up = false;
  Draw(base, saved.r3.u32, g_pending_up);
}

void ScissorHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  capture::Call(0x825E5670, ctx, base, false);
  g_scissor[0] = ctx.r4.s32;
  g_scissor[1] = ctx.r5.s32;
  g_scissor[2] = ctx.r6.s32;
  g_scissor[3] = ctx.r7.s32;
  original(ctx, base);
}

void ClearHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  capture::Call(0x825EC990, ctx, base, false);
  PPCContext saved = ctx;
  original(ctx, base);
  if (native::Enabled()) Clear(base, saved);
}

void ResolveHook(PPCContext& __restrict ctx, uint8_t* base, GuestFn original) {
  capture::Call(0x825ED210, ctx, base, false);
  PPCContext saved = ctx;
  original(ctx, base);
  if (native::Enabled()) Resolve(base, saved);
}

struct Register {
  Register() {
    trace::SetHandler(0x825EE420, DrawIndexedHook);              // DrawIndexedVertices
    trace::SetHandler(0x825EE1E0, DrawAutoHook<0x825EE1E0>);     // DrawVertices
    trace::SetHandler(0x825EDF40, BeginVerticesHook);            // BeginVertices
    trace::SetHandler(0x825EE1C0, EndVerticesHook);              // EndVertices
    trace::SetHandler(0x825E5670, ScissorHook);                  // internal SetScissor
    trace::SetHandler(0x825EC990, ClearHook);                    // Clear
    trace::SetHandler(0x825ED210, ResolveHook);                  // Resolve
    trace::SetHandler(0x825E85C0, CallHook<0x825E85C0>);         // SetRenderTarget
    trace::SetHandler(0x825E8958, CallHook<0x825E8958>);         // SetDepthStencilSurface
    trace::SetHandler(0x825E7DB0, CallHook<0x825E7DB0>);         // viewport / surface params
  }
} g_register;


// GPU time per frame from timestamp queries (read back a few frames later, never waiting).
struct GpuTimer {
  ID3D11Query* disjoint = nullptr;
  ID3D11Query* begin = nullptr;
  ID3D11Query* end = nullptr;
  bool pending = false;
};
GpuTimer g_gpu_timers[4];
double g_gpu_ms_acc = 0.0;
uint32_t g_gpu_ms_count = 0;
bool g_gpu_frame_open = false;

void GpuFrameBoundary() {
  auto* dev = native::Device();
  auto* ctx = native::Context();
  if (!dev) return;
  GpuTimer& cur = g_gpu_timers[g_frame & 3];
  if (g_gpu_frame_open) {
    GpuTimer& prev = g_gpu_timers[(g_frame - 1) & 3];
    ctx->End(prev.end);
    ctx->End(prev.disjoint);
    prev.pending = true;
  }
  // collect the oldest
  if (cur.pending) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj;
    UINT64 t0 = 0, t1 = 0;
    if (ctx->GetData(cur.disjoint, &dj, sizeof(dj), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(cur.begin, &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        ctx->GetData(cur.end, &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !dj.Disjoint && dj.Frequency) {
      g_gpu_ms_acc += double(t1 - t0) * 1000.0 / double(dj.Frequency);
      ++g_gpu_ms_count;
    }
    cur.pending = false;
  }
  if (!cur.disjoint) {
    D3D11_QUERY_DESC qd{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    dev->CreateQuery(&qd, &cur.disjoint);
    qd.Query = D3D11_QUERY_TIMESTAMP;
    dev->CreateQuery(&qd, &cur.begin);
    dev->CreateQuery(&qd, &cur.end);
  }
  ctx->Begin(cur.disjoint);
  ctx->End(cur.begin);
  g_gpu_frame_open = true;
}

// Render thread: end of a guest frame (front buffer to the window, present, statistics).
// Render thread, between frames: a new internal resolution (GRAPHICS menu) drops the render
// targets so they are recreated at the new size; resolve targets follow on their next resolve.
void ApplyScaleChange() {
  if (g_frame % 30 != 0) return;  // settings are polled, not pushed
  tex::RefreshSettings();
  RefreshCrtSettings();
  RefreshStereoSettings();
  const float want = DesiredScale();
  if (want == g_scale) return;
  auto* ctx = native::Context();
  ctx->OMSetRenderTargets(0, nullptr, nullptr);
  for (auto& [key, rt] : g_rts) {
    SafeRelease(rt.srv);
    SafeRelease(rt.rtv);
    SafeRelease(rt.dsv);
    SafeRelease(rt.tex);
  }
  g_rts.clear();
  InvalidateBindings();
  Log("renderer: internal resolution scale %.3fx -> %.3fx", g_scale, want);
  g_scale = want;
}

void ExecuteEndFrame(Cmd& cmd) {
  ProfScope prof_(kProfFrame);
  ++g_frame;
  tex::Trim(g_frame);
  uint32_t w = 0, h = 0;
  ID3D11ShaderResourceView* srv = nullptr;
  if (cmd.has_front) {
    const tex::FetchInfo f = tex::Decode(cmd.front_fetch);
    srv = tex::FindResolved(f.base, &w, &h);
  }
  if (g_stereo_mode && g_st_pending) StereoSnapshot();  // the 3D scene was the last thing drawn
  if (srv && w && h) {
    native::ClearBackBuffer(0, 0, 0);
    if (!(g_crt_preset && CrtBlit(srv, w, h, PostEffect(g_frame))))
      if (!g_stereo_mode || !g_blit_vs || !StereoBlit(srv, w, h)) Blit(srv, w, h, g_stereo_mode ? 0 : PostEffect(g_frame));
  } else {
    native::ClearBackBuffer(0.3f, 0.0f, 0.3f);  // magenta-ish: no front buffer resolved
  }
  GpuFrameBoundary();
  if (g_frame % 300 == 1)
    Log("frame %llu: draws %u (skipped %u, shader failures %u) clears %u resolves %u, shaders %zu, rts %zu",
        static_cast<unsigned long long>(g_frame), g_stats.draws, g_stats.skipped, g_stats.failed_shader, g_stats.clears,
        g_stats.resolves, g_shaders.size(), g_rts.size());
  g_stats = {};
  if (g_frame % 300 == 0) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 / double(f.QuadPart) / 300.0;
    char buf[512];
    int n = std::snprintf(buf, sizeof(buf), "renderer cpu (ms/frame):");
    for (int i = 0; i < kProfCount; ++i)
      n += std::snprintf(buf + n, sizeof(buf) - n, " %s %.2f", kProfNames[i], double(g_prof[i]) * ms);
    Log("%s | game-thread capture %.2f | gpu %.2f | hashed %.1f MB uploaded %.1f MB cb uploads %.0f per frame, mid-frame vb updates %llu", buf,
        double(g_cap_prof) * ms, g_gpu_ms_count ? g_gpu_ms_acc / g_gpu_ms_count : -1.0, double(g_prof_bytes_hashed) / 300.0 / 1048576.0,
        double(g_prof_bytes_uploaded) / 300.0 / 1048576.0, double(g_prof_cb_uploads) / 300.0,
        (unsigned long long)g_prof_vb_midframe);
    g_prof_vb_midframe = 0;
    std::memset(g_prof, 0, sizeof(g_prof));
    g_prof_bytes_hashed = g_prof_bytes_uploaded = g_prof_cb_uploads = 0;
    g_gpu_ms_acc = 0.0;
    g_gpu_ms_count = 0;
    g_cap_prof = 0;
  }
  native::Present();
  g_st_have = g_st_pending = false;
  g_st_rt = g_st_depth_rt = nullptr;
  ApplyScaleChange();
}

void RenderThreadMain() {
  std::vector<std::unique_ptr<Cmd>> work;
  for (;;) {
    {
      std::unique_lock<std::mutex> lk(g_q.mu);
      g_q.cv.wait(lk, [] { return !g_q.pending.empty(); });
      work.swap(g_q.pending);
    }
    for (auto& c : work) {
      switch (c->type) {
        case Cmd::kDraw: ExecuteDraw(g_q_base, *c); break;
        case Cmd::kClear: ExecuteClear(*c); break;
        case Cmd::kResolve: ExecuteResolve(*c); break;
        case Cmd::kEndFrame:
          ExecuteEndFrame(*c);
          g_q.frames_done.fetch_add(1);
          g_q.done_cv.notify_all();
          break;
        case Cmd::kRelease:
          for (auto* p : c->release) p->Release();
          break;
      }
    }
    {
      std::lock_guard<std::mutex> lk(g_q.mu);
      g_q.cmds_done.fetch_add(work.size());
      for (auto& c : work) {
        c->consts.reset();
        if (g_q.free_list.size() < 4096) g_q.free_list.push_back(std::move(c));
      }
    }
    g_q.done_cv.notify_all();
    work.clear();
  }
}

}  // namespace

// Game thread: end of a guest frame. Hands the frame to the render thread and keeps the game at
// most one frame ahead of it.
void EndFrame(uint8_t* base, uint32_t front) {
  if (!Init()) return;
  g_q_base = base;
  ++g_cap_frame;
  TrimVertexCache();
  auto cmd = NewCmd(Cmd::kEndFrame);
  if (front) {
    cmd->has_front = true;
    for (int k = 0; k < 6; ++k) cmd->front_fetch[k] = Load32(base, front + guest::obj::kTextureFetch + 4 * k);
  }
  Push(std::move(cmd));
  Flush();
  const uint64_t submitted = ++g_q.frames_submitted;
  if (submitted > 1) {
    std::unique_lock<std::mutex> lk(g_q.mu);
    g_q.done_cv.wait(lk, [&] { return g_q.frames_done.load() + 1 >= submitted; });
  }
}

}  // namespace q4hle::render
