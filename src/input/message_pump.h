#pragma once

#include <windows.h>

// SA-MP reads keyboard messages at pump level, before window-procedure
// dispatch, so WndProc-side suppression never reaches it. The queue itself
// is filtered instead: TAB messages that policy says to hide are rewritten
// to WM_NULL right where they are retrieved. Loaded-before-SA-MP hook
// ordering puts this filter between the real queue and SA-MP's reader for
// both an IAT hook and a later inline hook; a WH_GETMESSAGE hook installed
// after SA-MP's covers that route as well.
namespace bm {

void HookMessagePump();

// Installed once the game window is known (after SA-MP set its hooks, so this
// one runs first in the WH_GETMESSAGE chain and neutralizes TAB before SA-MP's
// hook sees it).
void InstallGetMessageHook(HWND window);

// Called from DLL_PROCESS_DETACH.
void RemoveGetMessageHook();

}  // namespace bm
