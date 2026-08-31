#pragma once

#include <windows.h>

namespace bm {

struct Config {
    bool logEnabled = false;
    bool fpsHotkeyEnabled = true;
    UINT fpsHotkeyModifier = 0;
    UINT fpsHotkeyKey = 0;

    // [debug] switches. All default to off, i.e. everything enabled. They
    // exist to bisect a misbehaving setup: turn parts of the plugin off one
    // at a time and see which one a game stops tripping over. The borderless
    // conversion itself is deliberately not switchable - without it there is
    // nothing left of the plugin to test.
    bool disableWindowHook = false;     // the WndProc subclass
    bool disableInputFilters = false;   // Get(Async)KeyState, GetKeyboardState
    bool disableMessagePump = false;    // Peek/GetMessage, WH_GETMESSAGE
    bool disableCursorGuard = false;    // SetCursorPos
    bool disableDisplayGuard = false;   // ChangeDisplaySettings*
    bool disableGamePatches = false;    // NoFrameDelay, refresh rate, FPS overlay
    bool disableBorderlessStyle = false;  // the window restyle and geometry
    bool disableConversion = false;       // the fullscreen->windowed conversion

    // Declaring the process DPI aware is what keeps a windowed device valid on
    // a scaled display. On by default; here to switch off if it ever clashes.
    bool dpiAware = true;
};

// Reads BorderlessMode.ini next to the ASI, creating it with defaults when it
// is missing. Legacy [BorderlessMode] and [SABorderless] sections still win
// when the current section is absent.
void LoadConfig();
const Config& GetConfig();

// LoadConfig runs before the log file is open - it is what decides whether
// there is one at all - so the summary of what was loaded is written from
// here instead, once logging is up.
void LogConfigSummary();

// The FPS overlay state is toggled from the window procedure and read from the
// render hook, so it lives behind an interlocked flag rather than in Config.
bool ShowFpsOverlay();
// Flips the overlay and writes the new value back to the INI. Returns it.
bool ToggleFpsOverlay();

}  // namespace bm
