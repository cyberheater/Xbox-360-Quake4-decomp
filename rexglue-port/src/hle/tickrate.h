// q4hle - game tic rate. Quake 4 asks the engine for its tic rate (idCommon::GetUserCmdHz /
// GetUserCmdMSec; 60 Hz / 16 ms in single player, 30 Hz / 33 ms in multiplayer) and derives the
// game's time step, the async tic timer and the controller turning from it. Overriding those two
// functions runs the whole game at a higher tic rate.
#pragma once

namespace q4hle::tick {

// Configured single-player tic rate (60 or 120). Q4_TICRATE=120 selects 120 Hz.
int Hz();
// Display/vblank rate the null GPU should emulate (same as the tic rate).
int RefreshHz();

}  // namespace q4hle::tick
