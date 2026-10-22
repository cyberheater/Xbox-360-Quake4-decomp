// q4 - GRAPHICS page in the game's Options menu (q4_graphics.cpp).
#pragma once

#include <functional>
#include <string>

namespace q4gfx {

// Adds the GRAPHICS entry and page to guis/mainmenu.gui text. Returns false (text unchanged) if
// the file does not look like the expected one.
bool PatchMainMenu(std::string& text);

// The app registers how to switch fullscreen live (called on the game thread; the handler must
// defer the window change to the UI thread). Without one the setting applies at the next start.
void SetFullscreenHandler(std::function<void(bool)> handler);

}  // namespace q4gfx
