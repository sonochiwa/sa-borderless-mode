#include "input/cursor.h"

#include "core/hook.h"
#include "core/log.h"
#include "window/borderless.h"

#include <windows.h>

namespace bm {
namespace {

using SetCursorPosFn = BOOL (WINAPI*)(int x, int y);

SetCursorPosFn g_originalSetCursorPos = nullptr;

BOOL WINAPI HookedSetCursorPos(int x, int y) {
    if (!GameOwnsForeground()) {
        Log("SetCursorPos suppressed: x=%d y=%d foreground=0x%p", x, y,
            GetForegroundWindow());
        return TRUE;
    }
    return g_originalSetCursorPos(x, y);
}

}  // namespace

void HookSetCursorPos() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    InstallExportHook(user32, "SetCursorPos",
                      reinterpret_cast<void*>(&HookedSetCursorPos),
                      reinterpret_cast<void**>(&g_originalSetCursorPos));
}

}  // namespace bm
