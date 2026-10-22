#pragma once
#include <cstdint>

struct ImportFixupStats
{
    unsigned functions = 0; // IAT slots pointed at their thunk
    unsigned variables = 0; // IAT slots pointed at freshly allocated kernel storage
};

// Patches every import slot in the loaded image (must run after the image is copied into guest
// memory and after Xex2LoadImage has populated g_Xex2ImportRecords).
ImportFixupStats FixupImports(uint8_t* base);

// Thread-safe bump allocator over the zero-filled kernel data region. Memory is never freed.
// Returns a 64-byte aligned guest address.
uint32_t KernelAlloc(uint32_t size);

// Guest address of the fake module (LDR) entry returned via XexExecutableModuleHandle.
extern uint32_t g_xexModuleHandle;
