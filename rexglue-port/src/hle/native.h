// q4hle - native D3D11 renderer: device, swap chain and presentation.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11RenderTargetView;

namespace q4hle::native {

void SetEnabled(bool on);
// Shadow mode: render offscreen while Xenia's GPU drives the window (for A/B comparison).
void SetOffscreen(bool on);
bool Offscreen();
bool Enabled();

// The app provides the HWND lazily (the window exists only after presentation setup).
void SetWindowProvider(std::function<void*()> provider);

// Creates the device and swap chain on first use. Returns false if not possible yet.
bool EnsureDevice();
ID3D11Device* Device();
ID3D11DeviceContext* Context();

// Size of the swap chain back buffer (follows the window's client area).
uint32_t BackBufferWidth();
uint32_t BackBufferHeight();

// Ends the frame: the renderer has drawn (or not) into the back buffer; present it.
void Present();

// Clears the back buffer (used until the guest output is available).
void ClearBackBuffer(float r, float g, float b);
ID3D11RenderTargetView* BackBufferRTV();

// Writes a 24-bit BMP from 32-bit RGBA (bgra = false) or BGRA pixels.
void WriteBmp(const std::string& path, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t pitch, bool bgra);

}  // namespace q4hle::native
