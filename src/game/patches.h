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

}  // namespace bm
