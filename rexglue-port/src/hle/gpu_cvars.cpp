// q4hle - the GPU settings that q4.exe gets from the SDK's Xenos GPU plugin.
//
// q4hle does not load that plugin (hle_app.cpp replaces the GPU with a null one), so without
// these the cvars would not exist in q4hle: the GRAPHICS menu and the F10 menu could not set
// them, q4.toml values for them would be ignored, and saving q4.toml from q4hle would drop them.
// Same names, types and defaults as the plugin (rexsdk src/graphics). If the plugin is loaded
// anyway (Q4_HLE_SHADOW=1 / Q4_HLE_NATIVE=0) its duplicate registrations are ignored.

#include <rex/cvar.h>

REXCVAR_DEFINE_INT32(resolution_scale, 1, "GPU",
                     "Draw resolution scale for both X and Y axes (q4hle: 1-4, applies instantly)");
REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme");
REXCVAR_DEFINE_INT32(anisotropic_override, 3, "GPU",
                     "Forces anisotropic filtering for eligible textures.\n"
                     " -1 = No override\n"
                     "  0 = Disable anisotropic filtering\n"
                     "  1 = Force 1x anisotropic filtering\n"
                     "  2 = Force 2x anisotropic filtering\n"
                     "  3 = Force 4x anisotropic filtering\n"
                     "  4 = Force 8x anisotropic filtering\n"
                     "  5 = Force 16x anisotropic filtering");
REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync (q4.exe; q4hle uses q4hle_vsync)");
