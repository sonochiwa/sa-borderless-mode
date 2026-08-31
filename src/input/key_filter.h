#pragma once

#include <windows.h>

// The game and SA:MP poll the global key state every frame instead of relying
// on window messages alone. These filters make sure keys held while another
// process owns the foreground - the TAB of an Alt+Tab above all - never leak
// into the game when it regains focus.
namespace bm {

// Hooks GetKeyState, GetAsyncKeyState and GetKeyboardState.
void HookKeyStateApis();

// Snapshots every physically held key at the moment focus is regained and
// mutes it until it is released. Also starts the refocus grace window.
void MuteKeysHeldAtRefocus();

// For a short window after the game regains focus, TAB reads as released on
// every filtered input path. Queued messages and lagging key-state tables can
// deliver the TAB of an Alt+Tab long after the key was physically released,
// past every state-based mute.
bool TabRefocusGraceActive();

// True while Alt or a Windows key is physically down, as seen through the
// unhooked GetAsyncKeyState. False when that trampoline is unavailable.
bool AltOrWinHeldAsync();

// Window-message mute table, kept separate from the polled one: queued
// autorepeat WM_KEYDOWNs and the lagging GetKeyState table observe the release
// at different times.
bool IsMessageKeyMuted(int virtualKey);
void ClearMessageKeyMute(int virtualKey);

}  // namespace bm
