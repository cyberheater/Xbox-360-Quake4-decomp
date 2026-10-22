#pragma once
#include <cstdint>

// Direct3D 11 backend for the Xenos command processor: shader translation, render-target emulation
// (EDRAM is modelled as one texture per base address), resolves and presentation.

// Register file access provided by the command processor.
uint32_t GpuRegRead(uint32_t index);

// Program binding (IM_LOAD / IM_LOAD_IMMEDIATE). type 0 = vertex, 1 = pixel.
void RendererSetShaderAddress(uint32_t type, uint32_t physAddr, uint32_t sizeDwords);
void RendererSetShaderImmediate(uint32_t type, const uint32_t* words, uint32_t sizeDwords);

// A DRAW_INDX / DRAW_INDX_2. `indices` is the immediate index data (host order dwords) or null;
// for DMA draws `dmaBase` is the physical address and `dmaSizeEndian` the VGT_DMA_SIZE-style word.
void RendererDraw(uint32_t initiator, const uint32_t* immediate, uint32_t immediateDwords, uint32_t dmaBase,
                  uint32_t dmaSizeEndian);

// Called by VdSwap with the guest pointer to the front-buffer fetch constant.
void RendererSwap(uint32_t fetchConstGuestAddr);

// Host input sampled for the virtual Xbox pad: keyboard/mouse (when the game window is focused) and a real XInput pad.
struct HostPad
{
    uint16_t buttons = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
    int16_t thumb[4] = {}; // LX, LY, RX, RY
    bool active = false;
};
HostPad RendererPollInput();
