#pragma once

namespace bm {

// Hooks GTA's frame-output call so the built-in FPS counter can draw on top of
// everything the game and SA:MP render. Does nothing on an unknown executable.
void HookFrameOutput();

}  // namespace bm
