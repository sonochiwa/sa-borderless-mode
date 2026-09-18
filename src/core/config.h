#pragma once

#include <windows.h>

namespace bm {

struct Config {
    bool logEnabled = false;
    bool fpsHotkeyEnabled = true;
    // VK_MENU is Alt.
    UINT fpsHotkeyModifier = VK_MENU;
    UINT fpsHotkeyKey = 0;

    // [debug] switches. All default to off, i.e. everything enabled. They
    // exist to bisect a misbehaving setup: turn parts of the plugin off one
    // at a time and see which one a game stops tripping over. The borderless
    // conversion itself is deliberately not switchable - without it there is
    // nothing left of the plugin to test.
    // The WndProc subclass.
    bool disableWindowHook = false;
    // Get(Async)KeyState and GetKeyboardState.
    bool disableInputFilters = false;
    // Peek/GetMessage and WH_GETMESSAGE.
    bool disableMessagePump = false;
    // SetCursorPos.
    bool disableCursorGuard = false;
    // ChangeDisplaySettings*.
    bool disableDisplayGuard = false;
    // NoFrameDelay, refresh rate, FPS overlay.
    bool disableGamePatches = false;
    // The window restyle and geometry.
    bool disableBorderlessStyle = false;
    // The fullscreen to windowed conversion.
    bool disableConversion = false;

    // Declaring the process DPI aware is a precondition for a windowed device
    // on a scaled display, not a preference: without it the game shuts itself
    // down during startup above 100% scaling, and at 100% setting it changes
    // nothing. So it is always done, and this only exists to take it out of
    // the picture while diagnosing something else.
    bool disableDpiAware = false;

    // Ticks a line every 500 ms while the game runs. Tells a process that died
    // apart from one whose window messages merely stopped arriving.
    bool heartbeat = false;
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
