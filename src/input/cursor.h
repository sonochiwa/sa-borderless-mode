#pragma once

namespace bm {

// The game keeps warping the cursor to the window centre every frame. In
// borderless mode that steals the pointer from whatever the user switched to,
// so the calls are dropped while another process owns the foreground.
void HookSetCursorPos();

}  // namespace bm
