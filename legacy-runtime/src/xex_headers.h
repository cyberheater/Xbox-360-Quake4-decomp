#pragma once
#include <cstdint>
#include <vector>

// Keeps the raw XEX bytes so optional headers can be handed to the guest on request.
void XexHeadersInit(std::vector<uint8_t>&& xexBytes);

// Copies the optional header `key` into kernel memory (once) and returns its guest address,
// or 0 if the XEX has no such header. Inline values are returned as a 4-byte big-endian word.
uint32_t XexHeaderFieldGuest(uint32_t key);
