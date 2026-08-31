#pragma once

namespace bm {

// Forces the next Reset through to the driver instead of recognizing it as
// redundant, after an event that invalidates what the device presents.
void RequireNextReset(const char* reason);

// Hooks Direct3DCreate9 in the d3d9.dll the game uses. IDirect3D9::CreateDevice
// and IDirect3DDevice9::Reset are hooked through their vtables as soon as the
// game creates those objects.
void InstallD3D9Hooks();

}  // namespace bm
