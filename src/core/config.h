#pragma once

#include <windows.h>

namespace bm {

struct Config {
    bool logEnabled = false;
    bool fpsHotkeyEnabled = true;
    UINT fpsHotkeyModifier = 0;
    UINT fpsHotkeyKey = 0;
};

// Reads BorderlessMode.ini next to the ASI, creating it with defaults when it
// is missing. Legacy [BorderlessMode] and [SABorderless] sections still win
// when the current section is absent.
void LoadConfig();
const Config& GetConfig();

// The FPS overlay state is toggled from the window procedure and read from the
// render hook, so it lives behind an interlocked flag rather than in Config.
bool ShowFpsOverlay();
// Flips the overlay and writes the new value back to the INI. Returns it.
bool ToggleFpsOverlay();

}  // namespace bm
