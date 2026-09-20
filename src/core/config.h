#pragma once

namespace bm {

// Reads BorderlessMode.ini next to the plugin, creating it from the copy
// compiled into the plugin when it is missing.
void LoadConfig();

// The word typed in game to show or hide the FPS counter. Letters and digits
// only, upper case; empty when the command is disabled.
const char* FpsCounterCommand();

// The FPS counter state is toggled on the game thread and read from the
// render hook, so it lives behind an interlocked flag.
bool ShowFpsCounter();
// Flips the counter and writes the new value back to the INI. Returns it.
bool ToggleFpsCounter();

}  // namespace bm
