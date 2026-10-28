// q4hle - application entry point. Replaces src/main.cpp for the q4hle executable.
//
// Q4HleApp is the normal Q4App with one change: unless Q4_HLE_NATIVE=0 is set, the SDK's Xenos
// GPU emulation is replaced by a null GPU (hle/null_gpu.cpp) and frames are presented by q4hle's own D3D11
// renderer (hle/native.cpp) from the hooked D3D functions.

#include "generated/default/q4_init.h"

#include <cstdlib>

#include "q4_app.h"
#include "q4_graphics.h"
#include "hle/native.h"
#include "hle/null_gpu.h"
#include "hle/tickrate.h"

namespace {

bool NativeEnabled() {
  const char* e = std::getenv("Q4_HLE_NATIVE");
  return !(e && *e == '0');
}

class Q4HleApp final : public Q4App {
 public:
  explicit Q4HleApp(rex::ui::WindowedAppContext& ctx) : Q4App(ctx, "q4", PPCImageConfig) {}

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Q4HleApp>(new Q4HleApp(ctx));
  }

 protected:
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (const char* sh = std::getenv("Q4_HLE_SHADOW"); sh && *sh == '1') {
      // Xenia's GPU keeps rendering and presenting; the native renderer runs offscreen alongside.
      q4hle::native::SetEnabled(true);
      q4hle::native::SetOffscreen(true);
      return;
    }
    if (!NativeEnabled()) return;
    if (q4hle::tick::Hz() > 60 && !std::getenv("Q4_SWAP_PHASE_MS")) {
      // VdSwap holds each frame until a fixed point after the vblank (8 ms by default, tuned for
      // 60 Hz). At 120 Hz that would be most of the interval, so present right away.
      _putenv("Q4_SWAP_PHASE_MS=0");
    }
    q4hle::native::SetEnabled(true);
    config.graphics = q4hle::CreateNullGpu();  // keeps the guest D3D happy; renders nothing
    config.gpu_plugin.clear();
  }

  void OnPostSetup() override {
    q4hle::native::SetWindowProvider([this]() -> void* {
      return window() ? window()->GetNativeWindowHandle() : nullptr;
    });
    // Hide the mouse pointer over the game window. Auto-hide: it reappears while the mouse moves
    // (for the F10 menu) and disappears again after a second. Keyboard/mouse mouse-look captures
    // and hides it as before. Q4_SHOW_CURSOR=1 keeps it visible.
    if (const char* e = std::getenv("Q4_SHOW_CURSOR"); !(e && *e == '1')) {
      app_context().CallInUIThreadDeferred([this] {
        if (!window()) return;
        window()->SetCursorAutoHideDelayMs(1000);
        window()->SetCursorVisibility(rex::ui::Window::CursorVisibility::kAutoHidden);
      });
    }
    // GRAPHICS menu -> DISPLAY MODE: switch on the UI thread, between frames.
    q4gfx::SetFullscreenHandler([this](bool on) {
      app_context().CallInUIThreadDeferred([this, on] {
        if (window()) window()->SetFullscreen(on);
      });
    });
  }
};

}  // namespace

REX_DEFINE_APP(q4, Q4HleApp::Create)
