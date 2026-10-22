// q4hle - sampling profiler for the recompiled game code (Q4_PROF=1). See profiler.cpp.
#pragma once

namespace q4hle::prof {
// Game thread: marks the end of a frame (called from the Swap hook). Starts the profiler on the
// first call when Q4_PROF=1.
void FrameEnd();
}  // namespace q4hle::prof
