// q4hle - guest textures for the native renderer. See textures.h.
//
// Xenos texture memory layout notes (from Xenia's texture_util / xenos.h, BSD licence):
//  - 2D tiled textures are stored in 32x32-block tiles; GetTiledOffset2D below is Xenia's
//    formula (originally from UModel). Linear textures have rows aligned to 256 bytes.
//  - Data is big-endian per the fetch constant's endian mode (8in16 for DXT, 8in32 for 32bpp).
//  - Component X of a texel is the lowest bits after the endian swap; the fetch constant swizzle
//    (applied in the generated shaders) maps components to the shader's xyzw.

#include "hle/textures.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "hle/guest.h"
#include "hle/log.h"
#include "hle/native.h"

#include <rex/cvar.h>

#include <atomic>
#include <cstdlib>
#include <string>

namespace q4hle::tex {
namespace {

int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bpb_log2) {
  pitch = (pitch + 31) & ~31u;
  int32_t macro = ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (bpb_log2 + 7);
  int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << bpb_log2;
  int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

struct FormatInfo {
  DXGI_FORMAT dxgi;
  DXGI_FORMAT dxgi_signed;
  uint32_t block;      // block width/height in texels (1 or 4)
  uint32_t bpb;        // bytes per block
  uint32_t swap_unit;  // unused
  bool expand_to_32;   // convert 16-bit packed formats to 8888 on upload
};

bool GetFormat(uint32_t fmt, FormatInfo& fi) {
  fi = {DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_UNKNOWN, 1, 4, 4, false};
  switch (fmt) {
    case 2:  // k_8
    case 8:  // k_8_A
    case 9:  // k_8_B
      fi.dxgi = DXGI_FORMAT_R8_UNORM; fi.dxgi_signed = DXGI_FORMAT_R8_SNORM; fi.bpb = 1; return true;
    case 3:  // k_1_5_5_5
    case 4:  // k_5_6_5
    case 15: // k_4_4_4_4
      fi.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; fi.bpb = 2; fi.expand_to_32 = true; return true;
    case 6:  // k_8_8_8_8
    case 14: // k_8_8_8_8_A
      fi.dxgi = DXGI_FORMAT_R8G8B8A8_UNORM; fi.dxgi_signed = DXGI_FORMAT_R8G8B8A8_SNORM; fi.bpb = 4; return true;
    case 7:  // k_2_10_10_10
      fi.dxgi = DXGI_FORMAT_R10G10B10A2_UNORM; fi.bpb = 4; return true;
    case 10: // k_8_8
      fi.dxgi = DXGI_FORMAT_R8G8_UNORM; fi.dxgi_signed = DXGI_FORMAT_R8G8_SNORM; fi.bpb = 2; return true;
    case 18: // k_DXT1
      fi.dxgi = DXGI_FORMAT_BC1_UNORM; fi.block = 4; fi.bpb = 8; return true;
    case 19: // k_DXT2_3
      fi.dxgi = DXGI_FORMAT_BC2_UNORM; fi.block = 4; fi.bpb = 16; return true;
    case 20: // k_DXT4_5
      fi.dxgi = DXGI_FORMAT_BC3_UNORM; fi.block = 4; fi.bpb = 16; return true;
    case 49: // k_DXN
      fi.dxgi = DXGI_FORMAT_BC5_UNORM; fi.dxgi_signed = DXGI_FORMAT_BC5_SNORM; fi.block = 4; fi.bpb = 16; return true;
    case 22: // k_24_8
    case 23: // k_24_8_FLOAT
      fi.dxgi = DXGI_FORMAT_R32_UINT; fi.bpb = 4; return true;  // depth as raw bits (rare)
    case 24: // k_16
      fi.dxgi = DXGI_FORMAT_R16_UNORM; fi.dxgi_signed = DXGI_FORMAT_R16_SNORM; fi.bpb = 2; return true;
    case 25: // k_16_16
      fi.dxgi = DXGI_FORMAT_R16G16_UNORM; fi.dxgi_signed = DXGI_FORMAT_R16G16_SNORM; fi.bpb = 4; return true;
    case 26: // k_16_16_16_16
      fi.dxgi = DXGI_FORMAT_R16G16B16A16_UNORM; fi.dxgi_signed = DXGI_FORMAT_R16G16B16A16_SNORM; fi.bpb = 8; return true;
    case 30: // k_16_FLOAT
      fi.dxgi = DXGI_FORMAT_R16_FLOAT; fi.bpb = 2; return true;
    case 31: // k_16_16_FLOAT
      fi.dxgi = DXGI_FORMAT_R16G16_FLOAT; fi.bpb = 4; return true;
    case 32: // k_16_16_16_16_FLOAT
      fi.dxgi = DXGI_FORMAT_R16G16B16A16_FLOAT; fi.bpb = 8; return true;
    case 36: // k_32_FLOAT
      fi.dxgi = DXGI_FORMAT_R32_FLOAT; fi.bpb = 4; return true;
    case 37: // k_32_32_FLOAT
      fi.dxgi = DXGI_FORMAT_R32G32_FLOAT; fi.bpb = 8; return true;
    case 38: // k_32_32_32_32_FLOAT
      fi.dxgi = DXGI_FORMAT_R32G32B32A32_FLOAT; fi.bpb = 16; return true;
    default:
      return false;
  }
}

inline uint32_t Swap(uint32_t v, uint32_t endian) {
  switch (endian & 3) {
    case 1: return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    case 2: return __builtin_bswap32(v);
    case 3: return (v >> 16) | (v << 16);
    default: return v;
  }
}

struct Entry {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  uint64_t hash = 0;
  uint64_t quick = 0;  // sparse sample hash, checked every frame
  uint64_t checked_frame = ~0ull;
  bool failed = false;
  bool logged = false;
  uint64_t last_used = 0;
};

struct Resolved {
  ID3D11Texture2D* tex;
  ID3D11ShaderResourceView* srv;
  uint32_t w, h;
  uint64_t frame;  // last frame it was written by a resolve
};
uint64_t g_current_frame = 0;

std::unordered_map<uint64_t, Entry> g_cache;
std::unordered_map<uint32_t, Resolved> g_resolved;
std::unordered_map<uint64_t, ID3D11SamplerState*> g_samplers;
ID3D11ShaderResourceView* g_white2d = nullptr;
ID3D11ShaderResourceView* g_whitecube = nullptr;
ID3D11ShaderResourceView* g_white3d = nullptr;
ID3D11ShaderResourceView* g_white1d = nullptr;
uint32_t g_unknown_logged[64] = {};

void MakePlaceholders() {
  if (g_white2d) return;
  auto* dev = native::Device();
  const uint32_t white[6] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF};
  D3D11_SUBRESOURCE_DATA sd[6];
  for (auto& s : sd) s = {white, 4, 4};
  D3D11_TEXTURE2D_DESC td{};
  td.Width = td.Height = 1;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ID3D11Texture2D* t = nullptr;
  dev->CreateTexture2D(&td, sd, &t);
  dev->CreateShaderResourceView(t, nullptr, &g_white2d);
  t->Release();
  td.ArraySize = 6;
  td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
  dev->CreateTexture2D(&td, sd, &t);
  dev->CreateShaderResourceView(t, nullptr, &g_whitecube);
  t->Release();
  D3D11_TEXTURE3D_DESC t3{1, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_SUBRESOURCE_DATA s3{white, 4, 4};
  ID3D11Texture3D* t3d = nullptr;
  dev->CreateTexture3D(&t3, &s3, &t3d);
  dev->CreateShaderResourceView(t3d, nullptr, &g_white3d);
  t3d->Release();
  D3D11_TEXTURE1D_DESC t1{1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  ID3D11Texture1D* t1d = nullptr;
  dev->CreateTexture1D(&t1, &s3, &t1d);
  dev->CreateShaderResourceView(t1d, nullptr, &g_white1d);
  t1d->Release();
}

ID3D11ShaderResourceView* Placeholder(xenos::TexDim dim) {
  MakePlaceholders();
  switch (dim) {
    case xenos::TexDim::kCube: return g_whitecube;
    case xenos::TexDim::k3D: return g_white3d;
    case xenos::TexDim::k1D: return g_white1d;
    default: return g_white2d;
  }
}

uint32_t Log2(uint32_t v) {
  uint32_t r = 0;
  while ((1u << r) < v) ++r;
  return r;
}
uint32_t NextPow2(uint32_t v) { return 1u << Log2(v); }
uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

uint32_t PackedMipLevel(uint32_t w, uint32_t h) {
  const uint32_t l = Log2(std::min(w, h));
  return l > 4 ? l - 4 : 0;
}

// Block offset of mip `mip` inside the packed mip tail (Xenia's GetPackedMipOffset, 2D only).
void PackedMipOffset(uint32_t w, uint32_t h, const FormatInfo& fi, uint32_t mip, uint32_t& xb, uint32_t& yb) {
  const uint32_t lw = Log2(w), lh = Log2(h), ls = std::min(lw, lh);
  xb = yb = 0;
  if (ls > 4 + mip) return;
  const uint32_t base = ls > 4 ? ls - 4 : 0;
  const uint32_t pm = mip - base;
  if (pm < 3) {
    if (lw > lh) yb = 16 >> pm;
    else xb = 16 >> pm;
  } else {
    if (lw > lh) xb = (1u << (lw - base)) >> (pm - 2);
    else yb = (1u << (lh - base)) >> (pm - 2);
  }
  xb /= fi.block;
  yb /= fi.block;
}

struct LevelLoc {
  uint32_t phys = 0;
  uint32_t pitch_blocks = 0;  // row pitch in blocks (tile-aligned for tiled data)
  uint32_t row_bytes = 0;     // linear row pitch
  uint32_t xoff = 0, yoff = 0;
  uint32_t face_stride = 0;   // bytes between cube faces / array slices at this level
};

// Where level `level` of the texture is stored (after Xenia's GetGuestTextureLayout).
bool Locate(const FetchInfo& f, const FormatInfo& fi, uint32_t level, LevelLoc& loc) {
  const bool packed = (f.dword[5] >> 11) & 1;
  const uint32_t packed_level = packed ? PackedMipLevel(f.width, f.height) : 0xFFFFFFFFu;
  const uint32_t faces = f.dimension == 3 ? 6 : 1;
  auto level_pitch_blocks = [&](uint32_t k) { return AlignUp(AlignUp(std::max(NextPow2(f.width) >> k, 1u), fi.block) / fi.block, 32); };
  auto level_rows = [&](uint32_t k) { return AlignUp(AlignUp(std::max(NextPow2(f.height) >> k, 1u), fi.block) / fi.block, 32); };
  auto level_row_bytes = [&](uint32_t k) {
    uint32_t rb = level_pitch_blocks(k) * fi.bpb;
    return f.tiled ? rb : AlignUp(rb, 256);
  };
  if (level == 0) {
    const uint32_t pb = std::max<uint32_t>(AlignUp(f.pitch, fi.block) / fi.block, 1);
    loc.phys = f.base;
    loc.pitch_blocks = f.tiled ? AlignUp(pb, 32) : pb;
    loc.row_bytes = f.tiled ? loc.pitch_blocks * fi.bpb : AlignUp(pb * fi.bpb, 256);
    const uint32_t hb = AlignUp(f.height, fi.block) / fi.block;
    loc.face_stride = AlignUp(AlignUp(loc.pitch_blocks, 32) * fi.bpb * AlignUp(hb, 32), 4096);
    if (packed_level == 0) PackedMipOffset(f.width, f.height, fi, 0, loc.xoff, loc.yoff);
    return true;
  }
  if (!f.mip) return false;
  const uint32_t stored = std::min(level, packed_level);
  uint32_t offset = 0;
  for (uint32_t k = 1; k < stored; ++k) offset += AlignUp(level_row_bytes(k) * level_rows(k), 4096) * faces;
  const uint32_t s = stored == 0 ? 0 : stored;
  loc.phys = f.mip + offset;
  loc.pitch_blocks = level_pitch_blocks(s);
  loc.row_bytes = level_row_bytes(s);
  loc.face_stride = AlignUp(loc.row_bytes * level_rows(s), 4096);
  if (level >= packed_level) PackedMipOffset(f.width, f.height, fi, level, loc.xoff, loc.yoff);
  return true;
}

// Decodes one level of one face into tightly packed host data of `w` x `h` texels.
void DecodeLevel(const uint8_t* base, const FetchInfo& f, const FormatInfo& fi, const LevelLoc& loc, uint32_t face,
                 uint32_t w, uint32_t h, std::vector<uint8_t>& out, uint32_t& out_row_pitch) {
  const uint32_t wb = (w + fi.block - 1) / fi.block, hb = (h + fi.block - 1) / fi.block;
  const uint32_t bpb_log2 = Log2(fi.bpb);
  const uint8_t* src = guest::PhysPtr(base, loc.phys + face * loc.face_stride);
  const uint32_t out_bpb = fi.expand_to_32 ? 4 : fi.bpb;
  out_row_pitch = wb * out_bpb;
  out.assign(size_t(out_row_pitch) * hb, 0);
  for (uint32_t by = 0; by < hb; ++by) {
    for (uint32_t bx = 0; bx < wb; ++bx) {
      const uint32_t x = bx + loc.xoff, y = by + loc.yoff;
      const uint32_t off = f.tiled ? uint32_t(TiledOffset2D(int32_t(x), int32_t(y), loc.pitch_blocks, bpb_log2))
                                   : y * loc.row_bytes + x * fi.bpb;
      const uint8_t* s = src + off;
      uint8_t* d = &out[size_t(by) * out_row_pitch + size_t(bx) * out_bpb];
      if (fi.bpb >= 4) {
        for (uint32_t i = 0; i < fi.bpb; i += 4) {
          uint32_t v;
          std::memcpy(&v, s + i, 4);
          v = Swap(v, f.endian);
          std::memcpy(d + i, &v, 4);
        }
      } else if (fi.bpb == 2) {
        uint16_t t;
        if (f.endian == 1) {
          uint16_t hw;
          std::memcpy(&hw, s, 2);
          t = __builtin_bswap16(hw);
        } else {
          uint32_t v;
          std::memcpy(&v, src + (off & ~3u), 4);
          v = Swap(v, f.endian);
          t = (off & 2) ? uint16_t(v >> 16) : uint16_t(v);
        }
        if (fi.expand_to_32) {
          uint32_t r, g, b, a;
          if (f.format == 4) {
            r = (t & 31) * 255 / 31; g = ((t >> 5) & 63) * 255 / 63; b = (t >> 11) * 255 / 31; a = 255;
          } else if (f.format == 3) {
            r = (t & 31) * 255 / 31; g = ((t >> 5) & 31) * 255 / 31; b = ((t >> 10) & 31) * 255 / 31; a = (t >> 15) ? 255 : 0;
          } else {
            r = (t & 15) * 17; g = ((t >> 4) & 15) * 17; b = ((t >> 8) & 15) * 17; a = (t >> 12) * 17;
          }
          const uint32_t px = r | (g << 8) | (b << 16) | (a << 24);
          std::memcpy(d, &px, 4);
        } else {
          std::memcpy(d, &t, 2);
        }
      } else {
        uint32_t v;
        std::memcpy(&v, src + (off & ~3u), 4);
        v = Swap(v, f.endian);
        d[0] = uint8_t(v >> (8 * (off & 3)));
      }
    }
  }
}

uint32_t SurfaceBytes(const FetchInfo& f, const FormatInfo& fi) {
  LevelLoc loc;
  Locate(f, fi, 0, loc);
  return loc.face_stride;
}

uint64_t SampleHash(const uint8_t* base, uint32_t phys, uint32_t bytes) {
  const uint8_t* p = guest::PhysPtr(base, phys);
  uint64_t h = 1469598103934665603ull ^ bytes;
  const uint32_t words = bytes / 4;
  // Small textures are hashed completely (font pages and similar change in small regions).
  const uint32_t step = words <= (64u << 10) ? 1 : words / 4096;
  for (uint32_t i = 0; i < words; i += step) {
    uint32_t v;
    std::memcpy(&v, p + size_t(i) * 4, 4);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// 64 evenly spaced 8-byte samples plus the first and last 64 bytes.
uint64_t QuickHash(const uint8_t* base, uint32_t phys, uint32_t bytes) {
  const uint8_t* p = guest::PhysPtr(base, phys);
  uint64_t h = 1469598103934665603ull ^ bytes;
  auto mix = [&](uint32_t off, uint32_t n) {
    for (uint32_t i = 0; i + 8 <= n; i += 8) {
      uint64_t v;
      std::memcpy(&v, p + off + i, 8);
      h = (h ^ v) * 1099511628211ull;
    }
  };
  if (bytes < 1024) {
    mix(0, bytes);
    return h;
  }
  mix(0, 64);
  mix(bytes - 64, 64);
  const uint32_t step = (bytes / 64) & ~7u;
  for (uint32_t i = 0; i < 64; ++i) mix(i * step, 8);
  return h;
}

}  // namespace

// Texture objects in the 0xE0000000 physical heap can carry the CPU (virtual) address; that heap
// is mapped 4 KB above its physical range, so convert to the physical address the GPU sees.
static uint32_t ToPhysical(uint32_t a) {
  return a >= 0xE0000000u ? ((a & 0x1FFFFFFFu) + 0x1000u) & 0x1FFFFFFFu : a & 0x1FFFFFFFu;
}

FetchInfo Decode(const uint32_t d[6]) {
  FetchInfo f{};
  std::memcpy(f.dword, d, 24);
  f.signs = (d[0] >> 2) & 0xFF;
  f.pitch = ((d[0] >> 22) & 0x1FF) << 5;
  f.tiled = (d[0] >> 31) & 1;
  f.format = d[1] & 0x3F;
  f.endian = (d[1] >> 6) & 3;
  f.base = ToPhysical(d[1] & 0xFFFFF000u);
  f.dimension = (d[5] >> 9) & 3;
  if (f.dimension == 2) {
    f.width = (d[2] & 0x7FF) + 1;
    f.height = ((d[2] >> 11) & 0x7FF) + 1;
    f.depth = ((d[2] >> 22) & 0x3FF) + 1;
  } else if (f.dimension == 0) {
    f.width = (d[2] & 0xFFFFFF) + 1;
    f.height = f.depth = 1;
  } else {
    f.width = (d[2] & 0x1FFF) + 1;
    f.height = ((d[2] >> 13) & 0x1FFF) + 1;
    f.depth = ((d[2] >> 26) & 0x3F) + 1;
  }
  f.swizzle = (d[3] >> 1) & 0xFFF;
  f.exp_adjust = int32_t(d[3] << 13) >> 26;  // bits 13-18 signed
  f.mip = (d[5] & 0xFFFFF000u) ? ToPhysical(d[5] & 0xFFFFF000u) : 0;
  return f;
}

ID3D11ShaderResourceView* GetTexture(const uint8_t* base, const FetchInfo& f, xenos::TexDim dim, uint64_t frame) {
  if (!f.base || f.width > 8192 || f.height > 8192) return Placeholder(dim);
  g_current_frame = frame;
  if (dim == xenos::TexDim::k2D) {
    auto it = g_resolved.find(f.base);
    // A resolve result stands in for guest memory while it is being refreshed; if nothing has
    // resolved there for a long time, the memory has likely been reused for other data.
    if (it != g_resolved.end() && frame - it->second.frame < 1800) return it->second.srv;
  }
  if (dim != xenos::TexDim::k2D && dim != xenos::TexDim::kCube) return Placeholder(dim);
  FormatInfo fi;
  if (!GetFormat(f.format, fi)) {
    if (f.format < 64 && !g_unknown_logged[f.format]++) Log("textures: unsupported format %u", f.format);
    return Placeholder(dim);
  }
  const bool is_signed = (f.signs & 0x55) == 0x55 && fi.dxgi_signed != DXGI_FORMAT_UNKNOWN;
  const uint64_t key = (uint64_t(f.base) << 32) ^ (uint64_t(f.dword[2]) << 8) ^ (f.format << 1) ^ (is_signed ? 1 : 0) ^
                       (uint64_t(dim == xenos::TexDim::kCube) << 63) ^ (uint64_t(f.tiled) << 62) ^
                       (uint64_t(f.pitch) << 40);
  Entry& e = g_cache[key];
  e.last_used = frame;
  if (e.failed) return Placeholder(dim);
  const uint32_t faces = dim == xenos::TexDim::kCube ? 6 : 1;
  const uint32_t face_bytes = (SurfaceBytes(f, fi) + 4095) & ~4095u;
  if (uint64_t(f.base & 0x1FFFFFFF) + uint64_t(face_bytes) * faces > 0x20000000ull) return Placeholder(dim);
  if (e.srv && e.checked_frame == frame) return e.srv;
  // Every frame a sparse sample of the data is compared; the full content hash is recomputed
  // when the sample changes and otherwise once every 16 frames (staggered per texture), so
  // textures that are rewritten in place are still picked up without hashing every texture
  // every frame.
  const uint32_t total = std::min<uint32_t>(face_bytes * faces, 16u << 20);
  const bool has_mip = f.mip && f.mip != f.base && uint64_t(f.mip & 0x1FFFFFFF) + uint64_t(face_bytes) * faces <= 0x20000000ull;
  uint64_t q = QuickHash(base, f.base, total);
  if (has_mip) q = q * 31 + QuickHash(base, f.mip, total);
  const bool due = ((frame + (key >> 7)) & 15) == 0;
  e.checked_frame = frame;
  if (e.srv && q == e.quick && !due) return e.srv;
  e.quick = q;
  uint64_t h = SampleHash(base, f.base, total);
  // The mip chain lives at its own address and can be streamed in after the base level.
  if (has_mip) h = h * 31 + SampleHash(base, f.mip, total);
  if (e.srv && h == e.hash) return e.srv;
  // (re)create
  if (e.srv) e.srv->Release();
  if (e.tex) e.tex->Release();
  e.srv = nullptr;
  e.tex = nullptr;
  e.hash = h;
  D3D11_TEXTURE2D_DESC td{};
  td.Width = f.width;
  td.Height = f.height;
  if (fi.block == 4) {  // BC textures need block-aligned dimensions for the top level
    td.Width = (td.Width + 3) & ~3u;
    td.Height = (td.Height + 3) & ~3u;
  }
  uint32_t levels = 1;
  if (f.mip) {
    const uint32_t max_level = (f.dword[4] >> 6) & 15;
    const uint32_t full = Log2(std::max(td.Width, td.Height)) + 1;
    levels = std::min(max_level + 1, full);
    if (levels < 1) levels = 1;
  }
  std::vector<std::vector<uint8_t>> data(size_t(levels) * faces);
  std::vector<D3D11_SUBRESOURCE_DATA> sd(size_t(levels) * faces);
  for (uint32_t face = 0; face < faces; ++face) {
    for (uint32_t l = 0; l < levels; ++l) {
      LevelLoc loc;
      const uint32_t lw = std::max(td.Width >> l, 1u), lh = std::max(td.Height >> l, 1u);
      uint32_t row_pitch = 0;
      auto& buf = data[face * levels + l];
      if (Locate(f, fi, l, loc)) {
        DecodeLevel(base, f, fi, loc, face, lw, lh, buf, row_pitch);
      } else {
        const uint32_t out_bpb = fi.expand_to_32 ? 4 : fi.bpb;
        row_pitch = ((lw + fi.block - 1) / fi.block) * out_bpb;
        buf.assign(size_t(row_pitch) * ((lh + fi.block - 1) / fi.block), 0xFF);
      }
      sd[face * levels + l] = {buf.data(), row_pitch, 0};
    }
  }
  td.MipLevels = levels;
  td.ArraySize = faces;
  td.Format = is_signed ? fi.dxgi_signed : fi.dxgi;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_IMMUTABLE;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  td.MiscFlags = faces == 6 ? D3D11_RESOURCE_MISC_TEXTURECUBE : 0;
  static int logged = 0;
  if (logged < 400 && !e.logged) {  // first upload only; re-uploads of changed content are not logged
    ++logged;
    e.logged = true;
    Log("texture: %08X fmt %u %ux%u levels %u faces %u tiled %d endian %u signs %02X swz %03X exp %d pitch %u packed %u mip %08X",
        f.base, f.format, f.width, f.height, levels, faces, int(f.tiled), f.endian, f.signs, f.swizzle, f.exp_adjust, f.pitch,
        (f.dword[5] >> 11) & 1, f.mip);
  }
  HRESULT hr = native::Device()->CreateTexture2D(&td, sd.data(), &e.tex);
  if (SUCCEEDED(hr)) hr = native::Device()->CreateShaderResourceView(e.tex, nullptr, &e.srv);
  if (FAILED(hr)) {
    Log("textures: create failed %08X (fmt %u %ux%u)", unsigned(hr), f.format, f.width, f.height);
    e.failed = true;
    return Placeholder(dim);
  }
  return e.srv;
}

// anisotropic_override (SDK cvar, GRAPHICS menu: texture filtering), refreshed by the renderer
// between frames. The SDK default is 3 (4x).
std::atomic<int32_t> g_aniso_override{3};

void RefreshSettings() {
  const std::string v = rex::cvar::GetFlagByName("anisotropic_override");
  g_aniso_override = v.empty() ? 3 : std::clamp(std::atoi(v.c_str()), -1, 5);
}

ID3D11SamplerState* GetSampler(const FetchInfo& f) {
  const uint32_t clamp = (f.dword[0] >> 10) & 0x1FF;
  const uint32_t filt = (f.dword[3] >> 19) & 0xFFF;
  const uint32_t lod = f.dword[4] & 0x3FFFFC;
  const uint32_t border = f.dword[5] & 3;
  // Anisotropic override (-1 = as the game asks, 0 = off, 1..5 = Xenos 1:1 .. 16:1), like the
  // SDK's GPU: applies to linear-filtered, mipmapped textures. Part of the key, so a change
  // takes effect at once.
  const int32_t aniso_ov = g_aniso_override.load(std::memory_order_relaxed);
  const uint64_t key = uint64_t(clamp) | (uint64_t(filt) << 9) | (uint64_t(lod) << 21) | (uint64_t(border) << 44) |
                       (uint64_t(aniso_ov + 1) << 46);
  auto it = g_samplers.find(key);
  if (it != g_samplers.end()) return it->second;
  auto addr = [](uint32_t m) {
    switch (m & 7) {
      case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
      case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
      case 2: case 4: return D3D11_TEXTURE_ADDRESS_CLAMP;
      case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
      default: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    }
  };
  D3D11_SAMPLER_DESC sd{};
  const uint32_t mag = (f.dword[3] >> 19) & 3, min = (f.dword[3] >> 21) & 3, mipf = (f.dword[3] >> 23) & 3;
  uint32_t aniso = (f.dword[3] >> 25) & 7;
  if (aniso_ov >= 0 && mag == 1 && min == 1 && (mipf == 0 || mipf == 1)) aniso = uint32_t(aniso_ov);
  if (aniso >= 2 && aniso <= 5) {
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 1u << (aniso - 1);
  } else {
    const bool lmag = mag == 1, lmin = min == 1, lmip = mipf == 1;
    sd.Filter = D3D11_FILTER((lmin ? 0x10 : 0) | (lmag ? 0x4 : 0) | (lmip ? 0x1 : 0));
    sd.MaxAnisotropy = 1;
  }
  sd.AddressU = addr(clamp);
  sd.AddressV = addr(clamp >> 3);
  sd.AddressW = addr(clamp >> 6);
  sd.MipLODBias = float(int32_t(f.dword[4] << 10) >> 22) / 32.0f;
  sd.MinLOD = float((f.dword[4] >> 2) & 15);
  sd.MaxLOD = mipf == 2 ? sd.MinLOD : float((f.dword[4] >> 6) & 15);
  if (sd.MaxLOD < sd.MinLOD) sd.MaxLOD = sd.MinLOD;
  if (sd.MaxLOD == 0.0f && mipf != 2) sd.MaxLOD = D3D11_FLOAT32_MAX;
  sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
  const float bc = border == 2 ? 1.0f : 0.0f;
  sd.BorderColor[0] = sd.BorderColor[1] = sd.BorderColor[2] = bc;
  sd.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
  ID3D11SamplerState* s = nullptr;
  native::Device()->CreateSamplerState(&sd, &s);
  g_samplers[key] = s;
  return s;
}

void SetResolved(uint32_t base, uint32_t width, uint32_t height, ID3D11Texture2D* texture, ID3D11ShaderResourceView* srv) {
  auto& r = g_resolved[base];
  r.frame = g_current_frame;
  if (r.srv == srv) return;
  if (r.srv) r.srv->Release();
  if (r.tex) r.tex->Release();
  texture->AddRef();
  srv->AddRef();
  r = {texture, srv, width, height, g_current_frame};
}

ID3D11ShaderResourceView* FindResolved(uint32_t base, uint32_t* width, uint32_t* height) {
  auto it = g_resolved.find(base);
  if (it == g_resolved.end() || g_current_frame - it->second.frame >= 1800) return nullptr;
  if (width) *width = it->second.w;
  if (height) *height = it->second.h;
  return it->second.srv;
}

void Trim(uint64_t frame) {
  g_current_frame = frame;
  if (frame % 600) return;
  size_t freed = 0;
  for (auto it = g_cache.begin(); it != g_cache.end();) {
    if (frame - it->second.last_used > 1800) {
      if (it->second.srv) it->second.srv->Release();
      if (it->second.tex) it->second.tex->Release();
      it = g_cache.erase(it);
      ++freed;
    } else {
      ++it;
    }
  }
  if (freed) Log("textures: evicted %zu unused textures, %zu cached", freed, g_cache.size());
}

void Shutdown() {}

}  // namespace q4hle::tex
