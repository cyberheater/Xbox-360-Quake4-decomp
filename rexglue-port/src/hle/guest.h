// q4hle - Quake 4's Xbox D3D (2005 XDK) device and object layouts, recovered from the recompiled
// D3D library (see docs/hle.md for how each offset was identified).
#pragma once

#include <cstdint>
#include <cstring>

namespace q4hle::guest {

// Host pointer for a guest virtual address (same arithmetic as recompiled code, including the
// 0x1000 host offset of the 0xE0000000 physical view on Windows).
inline const uint8_t* Ptr(const uint8_t* base, uint32_t addr) {
#if defined(_WIN32)
  return base + addr + (addr >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + addr;
#endif
}
inline uint8_t* Ptr(uint8_t* base, uint32_t addr) {
  return const_cast<uint8_t*>(Ptr(static_cast<const uint8_t*>(base), addr));
}
// Host pointer for a GPU physical address.
inline const uint8_t* PhysPtr(const uint8_t* base, uint32_t phys) { return Ptr(base, 0xA0000000u | (phys & 0x1FFFFFFFu)); }

inline uint32_t Load32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, Ptr(base, addr), 4);
  return __builtin_bswap32(v);
}
inline uint16_t Load16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, Ptr(base, addr), 2);
  return __builtin_bswap16(v);
}
inline void Store32(uint8_t* base, uint32_t addr, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(Ptr(base, addr), &value, 4);
}
inline float LoadF(const uint8_t* base, uint32_t addr) {
  uint32_t u = Load32(base, addr);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

constexpr uint32_t kDeviceDumpSize = 0x5000;

namespace dev {
// Command buffer
constexpr uint32_t kRingWrite = 0x000;  // current PM4 write pointer
constexpr uint32_t kRingLimit = 0x008;
// Dirty masks (64-bit, MSB first)
constexpr uint32_t kDirtyVsConsts = 0x010;
constexpr uint32_t kDirtyPsConsts = 0x018;
constexpr uint32_t kDirtyFetch = 0x020;
constexpr uint32_t kDirtyRegsA = 0x028;
constexpr uint32_t kDirtyRegsB = 0x030;
// GPU constant shadows
constexpr uint32_t kFetchConstants = 0x480;    // 32 x 6 dwords (register 0x4800)
constexpr uint32_t kVsFloatConstants = 0x780;  // 256 x float4 (register 0x4000)
constexpr uint32_t kPsFloatConstants = 0x1780; // 256 x float4 (register 0x4400)
constexpr uint32_t kBoolLoopConstants = 0x2780;// register 0x4900
// GPU register shadows: register r lives at group offset + 4 * (r - group base)
struct RegGroup {
  uint32_t first_reg, count, offset;
};
constexpr RegGroup kRegGroups[] = {
    {0x2000, 19, 0x2CC0}, {0x2100, 21, 0x2D0C}, {0x2180, 5, 0x2D60}, {0x2200, 12, 0x2D74},
    {0x2280, 21, 0x2DA4}, {0x2300, 38, 0x2DF8}, {0x2380, 16, 0x2E90},
};
constexpr uint32_t kVertexDeclaration = 0x2C90;
constexpr uint32_t kPixelShaderBound = 0x3290;
constexpr uint32_t kVertexShaderBound = 0x3294;
constexpr uint32_t kIndexBuffer = 0x30F4;
constexpr uint32_t kStreamOffset0 = 0x310C;  // + 8 * stream
constexpr uint32_t kStreamBuffer0 = 0x3110;  // + 8 * stream
constexpr uint32_t kStreamStrideDiv4 = 0x3190;  // byte per stream
constexpr uint32_t kVertexShaderSet = 0x4FD8;

// Offset of a register's shadow in the device, or 0 if not shadowed.
inline uint32_t RegOffset(uint32_t reg) {
  for (const auto& g : kRegGroups)
    if (reg >= g.first_reg && reg < g.first_reg + g.count) return g.offset + 4 * (reg - g.first_reg);
  return 0;
}
}  // namespace dev

namespace obj {
constexpr uint32_t kVsUcodeAddr = 0x28;   // guest address of (patched) vertex shader microcode
constexpr uint32_t kVsUcodeSize = 0x258;  // bytes
constexpr uint32_t kPsUcodeAddr = 0x0C;
constexpr uint32_t kPsUcodeSize = 0x3C;
// D3DBaseTexture: 0x10-byte resource header, then the 6-dword GPU texture fetch constant.
constexpr uint32_t kTextureFetch = 0x10;
}  // namespace obj

}  // namespace q4hle::guest
