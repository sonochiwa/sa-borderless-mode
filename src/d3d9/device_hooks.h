#pragma once

namespace bm {

// Forces the next Reset through to the driver instead of recognizing it as
// redundant, after an event that invalidates what the device presents.
void RequireNextReset();

// Hooks Direct3DCreate9 in the d3d9.dll the game uses. IDirect3D9::CreateDevice
// and IDirect3DDevice9::Reset are hooked through their vtables as soon as the
// game creates those objects.
void InstallD3D9Hooks();

// Presents the game's swap chain again without rendering anything, so the
// last frame stays on screen and the driver sees the game presenting. GTA
// stops rendering entirely while it is not the foreground application, and
// the NVIDIA App overlay treats a borderless window that has not presented
// for about three seconds as a game that quit: it tears down its in-game
// overlay, and the FPS counter comes back late, frozen, or not at all once
// the game renders again. Call it from the game thread while the game idles.
void PresentIdleFrame();

// Drops the device PresentIdleFrame() would use, when the window it presents
// to is being destroyed.
void ReleaseIdleDevice();

}  // namespace bm
