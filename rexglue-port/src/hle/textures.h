// q4hle - guest textures for the native renderer: decoding Xenos textures from guest memory,
// samplers from fetch constants, and render-to-texture results (resolves) that replace guest data.
#pragma once

#include <cstdint>

#include "hle/xenos_hlsl.h"

struct ID3D11ShaderResourceView;
struct ID3D11SamplerState;
struct ID3D11Texture2D;

namespace q4hle::tex {

struct FetchInfo {
  uint32_t dword[6];
  uint32_t format;     // xenos::TextureFormat
  uint32_t endian;
  uint32_t base;       // physical address of the base level
  uint32_t mip;        // physical address of the mip chain
  uint32_t width, height, depth;
  uint32_t pitch;      // texels (row pitch / 32 stored in the fetch constant)
  bool tiled;
  uint32_t dimension;  // xenos::DataDimension: 0 1D, 1 2D/stacked, 2 3D, 3 cube
  uint32_t swizzle;    // 12 bits
  uint32_t signs;      // 8 bits
  int exp_adjust;
};
FetchInfo Decode(const uint32_t fetch_host[6]);

// Shader resource view for a texture slot. Returns a placeholder if the texture can't be decoded.
ID3D11ShaderResourceView* GetTexture(const uint8_t* base, const FetchInfo& f, xenos::TexDim dim, uint64_t frame);
ID3D11SamplerState* GetSampler(const FetchInfo& f);
// Re-reads the texture filtering setting (anisotropic_override). Render thread, between frames.
void RefreshSettings();

// A render-target copy written by a guest Resolve to the texture at physical address `base`.
// Guest texture fetches of that address use it instead of guest memory.
void SetResolved(uint32_t base, uint32_t width, uint32_t height, ID3D11Texture2D* texture,
                 ID3D11ShaderResourceView* srv);
ID3D11ShaderResourceView* FindResolved(uint32_t base, uint32_t* width = nullptr, uint32_t* height = nullptr);

// Releases textures not used for a while (call once per frame).
void Trim(uint64_t frame);
void Shutdown();

}  // namespace q4hle::tex
