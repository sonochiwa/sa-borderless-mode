#pragma once

#include <windows.h>

namespace bm {

// Window styles the borderless window must never carry.
constexpr LONG kFrameStyleBits = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                 WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER |
                                 WS_DLGFRAME;
constexpr LONG kFrameExStyleBits = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                   WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

// True while the window the user is actually working in belongs to the game.
bool GameOwnsForeground();

// Strips the frame and stretches the window over the monitor it is on. Defers
// itself while the window is minimized or hidden, so a Reset arriving in the
// background cannot pull the game back over the user's window.
void ApplyBorderlessStyle(HWND window);

// True once the borderless geometry has been applied at least once.
bool BorderlessApplied();

// Set when the borderless geometry could not be applied because the window was
// minimized or hidden; the next restore re-applies it.
bool BorderlessPending();
void SetBorderlessPending(bool pending);

// Remains set from a real minimize/hide until the game actually becomes the
// foreground application. GTA/RenderWare can change the show state behind
// another application without ever activating the game.
bool WindowPutAway();
void SetWindowPutAway(bool putAway);

// Called when the game destroys the window this state described. GTA tears
// its window down and builds a new one while it settles on a video mode, and
// the new one has to be treated as a fresh first application.
void ResetBorderlessState();

}  // namespace bm
