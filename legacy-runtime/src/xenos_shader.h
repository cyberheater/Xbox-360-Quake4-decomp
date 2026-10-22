#pragma once
// Xenos (Xbox 360 GPU) shader microcode -> HLSL (SM5) translation.
#include <cstdint>
#include <string>

struct XenosShader
{
    bool vertex = false;
    bool ok = false;
    std::string hlsl;        // complete source for the stage (entry point: "main")
    std::string error;
    uint32_t texMask2D = 0;  // fetch constants sampled as 2D / 3D / cube
    uint32_t texMask3D = 0;
    uint32_t texMaskCube = 0;
    uint64_t vfetchSlots[2] = {}; // vertex-fetch slots used (96 bits)
    uint32_t slotStride[96] = {};    // dwords
    uint32_t slotExtent[96] = {};    // max (offset + 4) in dwords
    bool writesDepth = false;
    bool usesKill = false;
    uint32_t exportMask = 0; // vertex shader: interpolators written
    uint32_t numRegs = 0;
};

// `dwords` are the microcode words in host byte order. `count` includes the control-flow section.
bool TranslateXenosShader(bool vertex, const uint32_t* dwords, uint32_t count, XenosShader& out);

// Resource slots (shared with the renderer).
constexpr uint32_t kVfetchSlots = 96;     // VS: t0..t95 (ByteAddressBuffer)
constexpr uint32_t kVtxListSlot = 120;    // VS: StructuredBuffer<uint4>
constexpr uint32_t kTex2DBase = 0;        // PS: t0..t15
constexpr uint32_t kTex3DBase = 16;       // PS: t16..t31
constexpr uint32_t kTexCubeBase = 32;     // PS: t32..t47
