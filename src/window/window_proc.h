#pragma once

#include <windows.h>

namespace bm {

// Subclasses the game window. Everything the plugin needs to react to -
// activation, minimize/restore, style changes, the FPS hotkey and the Alt+Tab
// keyboard fallout - is handled there.
void InstallWindowHook(HWND window);

// The window InstallWindowHook subclassed, or null once the game destroyed it.
HWND HookedWindow();

// Starts retrying the borderless geometry until the game has shown its own
// window. Used when ApplyBorderlessStyle deferred because the window was not
// on screen yet.
void ScheduleBorderlessRetry(HWND window);

}  // namespace bm
