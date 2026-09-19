#pragma once

namespace bm {

// Removes GTA's fixed frame delay. Safe to call repeatedly: it re-applies the
// patch whenever something restored the original bytes, and does nothing when
// the executable is not GTA SA 1.0 US.
//
// Must only be called from the game thread at a point where the patched
// function is not on any stack (the CreateDevice/Reset and ApplyVideoMode
// hooks), never from the initialization thread.
void ApplyNoFrameDelay();

// Writes the current desktop refresh rate into the game's video-mode global,
// so the mode GTA rebuilds matches the monitor. Returns false when skipped.
bool UpdateGameRefreshRate();

// Keeps the refresh rate and the NoFrameDelay patch alive across in-game
// video setting changes.
void HookApplyVideoMode();

// Marks the game as focused again, the way GTA's own window procedure does on
// WM_ACTIVATE and WM_SETFOCUS. Call it from the window hook whenever the game
// takes focus back: the plugin swallows WM_SETFOCUS, and SAMPGraphicRestore.asi
// NOPs the WM_ACTIVATE write, so between the two the flag could otherwise stay
// clear forever and leave the game sleeping in its unfocused idle loop.
//
// Does nothing when the flag is already set, when game patches are disabled,
// or when the executable is not GTA SA 1.0 US.
void RestoreGameInFocus();

}  // namespace bm
