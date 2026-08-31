#pragma once

namespace bm {

// Remembers the desktop mode the game started on.
void CaptureDesktopMode();

// Someone (game, driver, another process) may still switch the real display
// mode behind the windowed device; put the desktop mode back immediately.
void RestoreDesktopMode(const char* reason);

// While borderless is active the desktop mode must stay untouched. GTA SA
// (and the AppCompat shims around d3d9) still believe the game runs exclusive
// fullscreen and switch the display mode, causing slow alt-tabs with monitor
// re-syncs. Requests are forwarded with the desktop mode substituted in.
void HookChangeDisplaySettings();

}  // namespace bm
