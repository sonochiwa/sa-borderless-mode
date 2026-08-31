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

}  // namespace bm
