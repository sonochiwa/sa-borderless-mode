#pragma once

#include <windows.h>

namespace bm {

// A word typed in game like a single-player cheat. The keys are taken from
// the message pump filter, which sees every keyboard message the game thread
// retrieves before the game or SA-MP does, chat box open or closed.

// The word to watch for. Letters and digits only, upper case; an empty word
// disables the command.
void SetTypedCommandWord(const char* word);

// Feeds one retrieved queue message. Only fresh key presses count.
void RecordTypedKey(const MSG* msg);

// True once for every complete word typed since the previous call.
bool ConsumeTypedCommand();

}  // namespace bm
