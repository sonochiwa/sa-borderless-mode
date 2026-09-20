#pragma once

#include <windows.h>

namespace bm {

struct Config {
    bool fpsHotkeyEnabled = true;
    // VK_MENU is Alt.
    UINT fpsHotkeyModifier = VK_MENU;
    UINT fpsHotkeyKey = VK_F11;
};

// Reads BorderlessMode.ini next to the plugin, creating it from the copy
// compiled into the plugin when it is missing.
void LoadConfig();
const Config& GetConfig();

// The FPS counter state is toggled from the window procedure and read from
// the render hook, so it lives behind an interlocked flag rather than in
// Config.
bool ShowFpsCounter();
// Flips the counter and writes the new value back to the INI. Returns it.
bool ToggleFpsCounter();

}  // namespace bm
