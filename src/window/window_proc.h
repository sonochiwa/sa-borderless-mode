#pragma once

#include <windows.h>

namespace bm {

// Subclasses the game window. Everything the plugin needs to react to -
// activation, minimize/restore, style changes, the FPS hotkey and the Alt+Tab
// keyboard fallout - is handled there.
void InstallWindowHook(HWND window);

}  // namespace bm
