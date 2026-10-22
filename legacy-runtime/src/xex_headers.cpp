#include "xex_headers.h"

#include <cstring>
#include <mutex>
#include <unordered_map>

#include "imports.h"
#include "memory.h"
#include "xex.h"

namespace
{
    std::mutex g_mutex;
    std::vector<uint8_t> g_bytes;
    std::unordered_map<uint32_t, uint32_t> g_cache; // key -> guest address (0 = known absent)
}

void XexHeadersInit(std::vector<uint8_t>&& xexBytes)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_bytes = std::move(xexBytes);
    g_cache.clear();
}

uint32_t XexHeaderFieldGuest(uint32_t key)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto cached = g_cache.find(key);
    if (cached != g_cache.end())
        return cached->second;

    uint32_t guestAddr = 0;
    if (!g_bytes.empty())
    {
        if (const void* p = getOptHeaderPtr(g_bytes.data(), key))
        {
            const uint32_t low = key & 0xFF;
            uint32_t size;
            if (low <= 1)
                size = 4;                                        // value stored inline in the header entry
            else if (low == 0xFF)
                size = _byteswap_ulong(*static_cast<const uint32_t*>(p)); // size-prefixed blob
            else
                size = low * 4;                                  // fixed-size struct, in dwords

            if (size < 4)
                size = 4;

            guestAddr = KernelAlloc(size);
            std::memcpy(g_guestBase + guestAddr, p, size);
        }
    }

    g_cache[key] = guestAddr;
    return guestAddr;
}
