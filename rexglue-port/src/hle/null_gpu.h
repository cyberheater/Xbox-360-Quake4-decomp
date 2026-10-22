// q4hle - minimal guest GPU (ring/fence/interrupt/vblank) for running with the native renderer.
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace q4hle {
std::unique_ptr<rex::system::IGraphicsSystem> CreateNullGpu();
}
