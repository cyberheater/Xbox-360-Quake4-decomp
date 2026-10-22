// q4hle - native renderer front end: receives the hooked D3D calls and draws them with D3D11.
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace q4hle::render {

// Guest frame finished; `front` is the guest front-buffer texture passed to Swap.
void EndFrame(uint8_t* base, uint32_t front);

}  // namespace q4hle::render
