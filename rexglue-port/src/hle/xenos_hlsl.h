// q4hle - Xenos shader microcode to HLSL (Shader Model 5) translator for the native renderer.
//
// Written from scratch for q4hle. Microcode layouts come from the SDK's ucode.h (Xenia); the
// instruction semantics follow Xenia's documentation of the Xenos ALU and fetch units.
//
// Binding model of the generated shaders (see renderer.cpp, which fills these):
//   b0  float4 c[256]      ALU float constants of the stage
//   b1  XeDraw              per-draw state: bool/loop constants, viewport transform, alpha test,
//                           per-texture-slot swizzle/sign info
//   t0..t31   textures by fetch-constant slot (Texture1D/2D/3D/Cube by use)
//   s0..s15   samplers, allocated per shader (map in TranslatedShader::sampler_slot)
//   t32..t127 (vertex shaders) vertex fetch constants 0..95 as ByteAddressBuffer, little-endian
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace q4hle::xenos {

enum class TexDim : uint8_t { kNone = 0, k1D, k2D, k3D, kCube };

struct TranslatedShader {
  bool ok = false;
  bool pixel = false;
  std::string hlsl;
  std::string error;        // unsupported features (shader still generated when possible)
  std::string disassembly;  // human-readable listing (also embedded as comments)
  uint32_t vfetch_mask[3] = {0, 0, 0};  // vertex fetch constants used (VS)
  uint16_t vf_stride_dw[96] = {};       // per fetch constant: stride in dwords (first full fetch)
  uint32_t vf_extent_dw[96] = {};       // per fetch constant: max (offset + element size) in dwords
  TexDim tex_dim[32] = {};              // texture fetch slots used and their dimension
  int8_t sampler_slot[32];              // texture slot -> sampler register, -1 if none
  uint32_t color_targets = 0;           // PS: bitmask of oC# written
  bool writes_depth = false;
  bool uses_kill = false;
  uint32_t max_temp = 0;
  TranslatedShader() {
    for (auto& s : sampler_slot) s = -1;
  }
};

struct TranslateOptions {
  // PS: register that receives the pixel position (SQ_CONTEXT_MISC.param_gen_pos) or -1.
  int param_gen_reg = -1;
  // PS: number of interpolators loaded into r0.. (min of VS exports and PS registers).
  uint32_t interpolator_count = 16;
};

// `ucode` is the shader microcode as host-endian dwords.
TranslatedShader Translate(const uint32_t* ucode, size_t dword_count, bool pixel,
                           const TranslateOptions& options);

// The HLSL declarations shared by both stages (constant buffer layouts); kept in sync with the
// renderer's C++ structs.
const char* CommonHlsl();

}  // namespace q4hle::xenos
