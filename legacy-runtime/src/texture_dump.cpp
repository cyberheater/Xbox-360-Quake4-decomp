// Xenos texture decoding for diagnostics: untiles, endian-swaps and decodes a fetch-constant
// texture and writes it as an (uncompressed) PNG. This is the same decode path the renderer
// will need, kept standalone so it can be checked by eye.
#include "texture_dump.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "memory.h"

namespace
{
    constexpr uint32_t kFmt_8 = 0x02;
    constexpr uint32_t kFmt_565 = 0x04;
    constexpr uint32_t kFmt_8888 = 0x06;
    constexpr uint32_t kFmt_DXT1 = 0x12;
    constexpr uint32_t kFmt_DXT3 = 0x13;
    constexpr uint32_t kFmt_DXT5 = 0x14;

    uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }

    // Offset (in bytes) of block (x, y) inside a tiled 2D surface. `pitch` is in blocks.
    uint32_t TiledOffset2D(uint32_t x, uint32_t y, uint32_t pitch, uint32_t log2Bpp)
    {
        const uint32_t macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (log2Bpp + 7);
        const uint32_t micro = ((x & 7) + ((y & 0xE) << 2)) << log2Bpp;
        const uint32_t offset = macro + ((micro & ~0xFu) << 1) + (micro & 0xF) + ((y & 1) << 4);
        return ((offset & ~0x1FFu) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
               (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
    }

    void SwapBytes(uint8_t* data, size_t size, uint32_t endian)
    {
        switch (endian)
        {
        case 1: // 8-in-16
            for (size_t i = 0; i + 1 < size; i += 2)
                std::swap(data[i], data[i + 1]);
            break;
        case 2: // 8-in-32
            for (size_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 3]);
                std::swap(data[i + 1], data[i + 2]);
            }
            break;
        case 3: // 16-in-32
            for (size_t i = 0; i + 3 < size; i += 4)
            {
                std::swap(data[i], data[i + 2]);
                std::swap(data[i + 1], data[i + 3]);
            }
            break;
        default:
            break;
        }
    }

    void Rgb565(uint16_t c, uint8_t* out)
    {
        out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
        out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
        out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
    }

    // Decodes the colour half (8 bytes) of a DXT block to 16 RGBA texels.
    void DecodeColour(const uint8_t* b, uint8_t* rgba, bool opaque)
    {
        const uint16_t c0 = static_cast<uint16_t>(b[0] | (b[1] << 8));
        const uint16_t c1 = static_cast<uint16_t>(b[2] | (b[3] << 8));
        uint8_t pal[4][4];
        Rgb565(c0, pal[0]);
        Rgb565(c1, pal[1]);
        pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
        if (c0 > c1 || opaque)
        {
            for (int i = 0; i < 3; i++)
            {
                pal[2][i] = static_cast<uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
                pal[3][i] = static_cast<uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
            }
        }
        else
        {
            for (int i = 0; i < 3; i++)
            {
                pal[2][i] = static_cast<uint8_t>((pal[0][i] + pal[1][i]) / 2);
                pal[3][i] = 0;
            }
            pal[3][3] = 0;
        }

        const uint32_t bits = b[4] | (b[5] << 8) | (b[6] << 16) | (static_cast<uint32_t>(b[7]) << 24);
        for (int i = 0; i < 16; i++)
            std::memcpy(rgba + i * 4, pal[(bits >> (i * 2)) & 3], 4);
    }

    void DecodeBc4(const uint8_t* block, uint8_t* out)
    {
        uint8_t a[8];
        a[0] = block[0];
        a[1] = block[1];
        if (a[0] > a[1])
        {
            for (int i = 1; i < 7; i++)
                a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
        }
        else
        {
            for (int i = 1; i < 5; i++)
                a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
            a[6] = 0;
            a[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; i++)
            bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
        for (int i = 0; i < 16; i++)
            out[i] = a[(bits >> (3 * i)) & 7];
    }

    void DecodeDxt(const uint8_t* block, uint32_t fmt, uint8_t* rgba)
    {
        if (fmt == 49) // DXN: two BC4 blocks (x, y)
        {
            uint8_t x[16], y[16];
            DecodeBc4(block, x);
            DecodeBc4(block + 8, y);
            for (int i = 0; i < 16; i++)
            {
                rgba[i * 4 + 0] = x[i];
                rgba[i * 4 + 1] = y[i];
                rgba[i * 4 + 2] = 0;
                rgba[i * 4 + 3] = 255;
            }
            return;
        }
        if (fmt == kFmt_DXT1)
        {
            DecodeColour(block, rgba, false);
            return;
        }

        DecodeColour(block + 8, rgba, true);
        if (fmt == kFmt_DXT3)
        {
            for (int i = 0; i < 16; i++)
            {
                const uint8_t v = (block[i / 2] >> ((i & 1) * 4)) & 0xF;
                rgba[i * 4 + 3] = static_cast<uint8_t>(v * 17);
            }
        }
        else
        {
            uint8_t a[8];
            a[0] = block[0];
            a[1] = block[1];
            if (a[0] > a[1])
            {
                for (int i = 1; i < 7; i++)
                    a[i + 1] = static_cast<uint8_t>(((7 - i) * a[0] + i * a[1]) / 7);
            }
            else
            {
                for (int i = 1; i < 5; i++)
                    a[i + 1] = static_cast<uint8_t>(((5 - i) * a[0] + i * a[1]) / 5);
                a[6] = 0;
                a[7] = 255;
            }
            uint64_t bits = 0;
            for (int i = 0; i < 6; i++)
                bits |= static_cast<uint64_t>(block[2 + i]) << (8 * i);
            for (int i = 0; i < 16; i++)
                rgba[i * 4 + 3] = a[(bits >> (3 * i)) & 7];
        }
    }

    uint32_t Crc32(const uint8_t* d, size_t n, uint32_t crc = 0)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; i++)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; k++)
                    c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        crc = ~crc;
        for (size_t i = 0; i < n; i++)
            crc = table[(crc ^ d[i]) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }

    void Put32(std::vector<uint8_t>& v, uint32_t x)
    {
        v.push_back(static_cast<uint8_t>(x >> 24));
        v.push_back(static_cast<uint8_t>(x >> 16));
        v.push_back(static_cast<uint8_t>(x >> 8));
        v.push_back(static_cast<uint8_t>(x));
    }

    void Chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
    {
        Put32(out, static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        out.insert(out.end(), body.begin(), body.end());
        Put32(out, Crc32(body.data(), body.size()));
    }

    bool WritePng(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h)
    {
        std::vector<uint8_t> raw;
        raw.reserve(static_cast<size_t>(h) * (w * 4 + 1));
        for (uint32_t y = 0; y < h; y++)
        {
            raw.push_back(0); // filter: none
            raw.insert(raw.end(), rgba + static_cast<size_t>(y) * w * 4, rgba + static_cast<size_t>(y + 1) * w * 4);
        }

        // zlib stream made of stored (uncompressed) deflate blocks.
        std::vector<uint8_t> z = { 0x78, 0x01 };
        size_t pos = 0;
        while (pos < raw.size())
        {
            const size_t n = std::min<size_t>(65535, raw.size() - pos);
            const bool last = pos + n == raw.size();
            z.push_back(last ? 1 : 0);
            z.push_back(static_cast<uint8_t>(n));
            z.push_back(static_cast<uint8_t>(n >> 8));
            z.push_back(static_cast<uint8_t>(~n));
            z.push_back(static_cast<uint8_t>((~n) >> 8));
            z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
            pos += n;
        }
        uint32_t a = 1, b = 0;
        for (uint8_t c : raw)
        {
            a = (a + c) % 65521;
            b = (b + a) % 65521;
        }
        Put32(z, (b << 16) | a);

        std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        std::vector<uint8_t> ihdr;
        Put32(ihdr, w);
        Put32(ihdr, h);
        ihdr.push_back(8);  // bit depth
        ihdr.push_back(6);  // RGBA
        ihdr.push_back(0);
        ihdr.push_back(0);
        ihdr.push_back(0);
        Chunk(png, "IHDR", ihdr);
        Chunk(png, "IDAT", z);
        Chunk(png, "IEND", {});

        FILE* f = std::fopen(path, "wb");
        if (!f)
            return false;
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
        return true;
    }
}

bool TextureDecode(const uint32_t fetch[6], std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH, const char* path)
{
    const uint32_t type = fetch[0] & 3;
    if (type != 2)
        return false;

    const uint32_t pitchTexels = ((fetch[0] >> 22) & 0x1FF) * 32;
    const bool tiled = (fetch[0] >> 31) & 1;
    const uint32_t fmt = fetch[1] & 0x3F;
    const uint32_t endian = (fetch[1] >> 6) & 3;
    const uint32_t base = (fetch[1] >> 12) << 12;
    const uint32_t width = (fetch[2] & 0x1FFF) + 1;
    const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
    outW = width;
    outH = height;

    if (width > 4096 || height > 4096 || base == 0)
        return false;

    uint32_t blockW = 1, blockH = 1, log2Bpp = 0;
    switch (fmt)
    {
    case kFmt_8: log2Bpp = 0; break;
    case kFmt_565: log2Bpp = 1; break;
    case kFmt_8888: log2Bpp = 2; break;
    case kFmt_DXT1: blockW = blockH = 4; log2Bpp = 3; break;
    case kFmt_DXT3:
    case kFmt_DXT5: blockW = blockH = 4; log2Bpp = 4; break;
    case 49: blockW = blockH = 4; log2Bpp = 4; break; // DXN
    default:
        std::printf("[tex] %s: unsupported format 0x%02X (%ux%u)\n", path, fmt, width, height);
        return false;
    }

    const uint32_t bw = (width + blockW - 1) / blockW;
    const uint32_t bh = (height + blockH - 1) / blockH;
    uint32_t pitchBlocks = AlignUp(bw, 32);
    if (pitchTexels / blockW > pitchBlocks)
        pitchBlocks = pitchTexels / blockW;
    const uint32_t rows = AlignUp(bh, 32);
    const uint32_t bytesPerBlock = 1u << log2Bpp;

    const uint32_t virt = guest::kPhysicalBegin + (base & 0x1FFFFFFF);
    const size_t surfaceBytes = static_cast<size_t>(pitchBlocks) * rows * bytesPerBlock;

    std::vector<uint8_t> raw(surfaceBytes);
    std::memcpy(raw.data(), g_guestBase + virt, surfaceBytes);
    SwapBytes(raw.data(), raw.size(), endian);

    rgba.assign(static_cast<size_t>(width) * height * 4, 0);
    uint8_t blockRgba[16 * 4];

    for (uint32_t by = 0; by < bh; by++)
    {
        for (uint32_t bx = 0; bx < bw; bx++)
        {
            const uint32_t off = tiled ? TiledOffset2D(bx, by, pitchBlocks, log2Bpp)
                                       : (by * pitchBlocks + bx) * bytesPerBlock;
            if (off + bytesPerBlock > raw.size())
                continue;
            const uint8_t* src = raw.data() + off;

            if (blockW == 4)
            {
                DecodeDxt(src, fmt, blockRgba);
                for (uint32_t py = 0; py < 4; py++)
                {
                    for (uint32_t px = 0; px < 4; px++)
                    {
                        const uint32_t x = bx * 4 + px, y = by * 4 + py;
                        if (x < width && y < height)
                            std::memcpy(&rgba[(static_cast<size_t>(y) * width + x) * 4], blockRgba + (py * 4 + px) * 4, 4);
                    }
                }
            }
            else
            {
                uint8_t* dst = &rgba[(static_cast<size_t>(by) * width + bx) * 4];
                if (fmt == kFmt_8)
                {
                    dst[0] = dst[1] = dst[2] = src[0];
                    dst[3] = 255;
                }
                else if (fmt == kFmt_565)
                {
                    Rgb565(static_cast<uint16_t>((src[0] << 8) | src[1]), dst);
                    dst[3] = 255;
                }
                else // 8888: natural channel order (byte 0 first); the fetch constant's swizzle orders it
                {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = src[3];
                }
            }
        }
    }

    return true;
}

bool TextureDump(const uint32_t fetch[6], const char* path)
{
    std::vector<uint8_t> rgba;
    uint32_t w = 0, h = 0;
    if (!TextureDecode(fetch, rgba, w, h, path))
        return false;
    const uint32_t swz = (fetch[3] >> 1) & 0xFFF;
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
    {
        uint8_t in[8] = { rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3], 0, 255, 0, 0 };
        for (int k = 0; k < 4; k++)
            rgba[i + k] = in[(swz >> (3 * k)) & 7];
    }
    return WritePng(path, rgba.data(), w, h);
}
bool WritePngRgba(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) { return WritePng(path, rgba, w, h); }
