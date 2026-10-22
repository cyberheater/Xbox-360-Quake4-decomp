#pragma once
#include <cstdint>

// Decodes the 2D texture described by a Xenos fetch constant (6 dwords, host byte order) and
// writes it as a PNG. Returns false for unsupported formats or bad constants.
bool TextureDump(const uint32_t fetch[6], const char* path);

// Decodes to canonical RGBA8 (R,G,B,A in byte order). `tag` is only used in diagnostics.
#include <vector>
bool TextureDecode(const uint32_t fetch[6], std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height, const char* tag);
bool WritePngRgba(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);
