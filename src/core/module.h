#pragma once

#include <windows.h>

namespace bm {

// The ASI module itself. Set once from DllMain.
void SetSelfModule(HMODULE module);
HMODULE SelfModule();

// Builds a path next to the ASI with its extension replaced, e.g. ".ini".
bool BuildSiblingPath(const wchar_t* extension, wchar_t* path, DWORD pathSize);

// This plugin patches user32 and d3d9 exports, subclasses the game window and
// registers a WH_GETMESSAGE hook, none of which can be withdrawn safely from
// DllMain. Pinning the module makes a FreeLibrary on it a no-op, so those
// hooks can never point into unmapped memory. It is the same protection the
// Windows compatibility engine applies as the IgnoreFreeLibrary shim, done
// deliberately instead of waiting for Windows to diagnose a crash first.
void PinSelf();

// Declares the game process DPI aware, early, before it creates its window.
//
// GTA SA declares nothing, so on a scaled display Windows virtualizes it: the
// process sees a 2048x1152 desktop on a 2560x1440 monitor at 125%. Exclusive
// fullscreen hides that, but a windowed device whose back buffer is the real
// monitor size on a virtualized desktop makes the game shut itself down during
// startup. Windows papers over this by eventually recording a HIGHDPIAWARE
// compatibility layer for the executable, which is why a fresh copy of a
// modpack fails and the same files in a folder that has been run a few times
// do not. Doing it ourselves removes the dependency on that.
//
// Returns true when awareness was set by this call.
bool MakeProcessDpiAware();

// The state that actually took effect. A "high DPI scaling override" set on
// gta_sa.exe wins over the call above, which still reports success, so this is
// the only trustworthy answer.
bool IsProcessDpiAwareNow();

}  // namespace bm
