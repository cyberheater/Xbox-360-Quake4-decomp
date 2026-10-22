// q4hle - frame capture: dumps the guest D3D device state at every draw of one frame so that the
// shader translator and state mapping can be developed and tested offline.
//
//   Q4_HLE_CAPTURE_FRAME=<n>   frame to capture (counted by the Swap hook)
//   Q4_TRACE_DIR=<dir>         output directory (capture.bin)
#pragma once

#include <cstdint>

#include <rex/ppc/context.h>

namespace q4hle::capture {

bool Active();
// Records a D3D call with its register arguments. For draws, also the device and shader ucode.
void Call(uint32_t fn, const PPCContext& ctx, uint8_t* base, bool is_draw);
void FrameEnd(uint64_t frame);

}  // namespace q4hle::capture
