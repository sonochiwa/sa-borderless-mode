#include "input/key_filter.h"

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "window/borderless.h"

#include <intrin.h>

#include <cstring>

namespace bm {
namespace {

using GetKeyStateFn = SHORT (WINAPI*)(int virtualKey);
using GetAsyncKeyStateFn = SHORT (WINAPI*)(int virtualKey);
using GetKeyboardStateFn = BOOL (WINAPI*)(PBYTE keyState);

GetKeyStateFn g_originalGetKeyState = nullptr;
GetAsyncKeyStateFn g_originalGetAsyncKeyState = nullptr;
GetKeyboardStateFn g_originalGetKeyboardState = nullptr;

// Keys that are still physically held at the moment the game regains the
// foreground (the TAB of an Alt+Tab released a moment after Alt, a mouse
// button used to click the window, keys typed in another app) must not leak
// in as fresh presses.
// Polled key-state APIs.
volatile LONG g_stickyMutedKeys[256] = {};
// Window keyboard messages.
volatile LONG g_msgMutedKeys[256] = {};

volatile DWORD g_lastRefocusTick = 0;

constexpr DWORD kTabRefocusGraceMs = 200;

void LogTabDownRead(const char* api, void* caller, bool suppressed) {
    static volatile DWORD lastLogTick = 0;
    DWORD now = GetTickCount();
    DWORD last = lastLogTick;
    if (last != 0 && now - last < 250) {
        return;
    }
    lastLogTick = now;
    char callerText[kCallerTextSize] = {};
    FormatCallerAddress(caller, callerText, sizeof(callerText));
    Log("TAB down read via %s caller=%s%s", api, callerText,
        suppressed ? " (suppressed)" : "");
}

// Decides whether a polled read must present the key as released.
// `reportedDown` is what the calling API itself sees for the key, so a
// source whose state lags behind the physical release keeps muting until
// it catches up. The flag is dropped once the async state agrees the key
// is up.
bool StickyMuteFilter(int virtualKey, bool reportedDown) {
    unsigned index = static_cast<unsigned>(virtualKey) & 0xFF;
    if (!g_stickyMutedKeys[index]) {
        return false;
    }
    if (reportedDown) {
        return true;
    }
    if (g_originalGetAsyncKeyState &&
        !(g_originalGetAsyncKeyState(static_cast<int>(index)) & 0x8000)) {
        InterlockedExchange(&g_stickyMutedKeys[index], 0);
    }
    return false;
}

// Polled key state is muted entirely while the foreground window belongs to
// another process, and re-armed on the transition back into the game.
bool ForegroundBelongsToGame() {
    LONG game = GameOwnsForeground() ? 1 : 0;

    static LONG lastLogged = 1;
    if (InterlockedExchange(&lastLogged, game) != game) {
        Log("polled input %s: foreground=0x%p",
            game ? "restored" : "muted", GetForegroundWindow());
        if (game) {
            MuteKeysHeldAtRefocus();
        }
    }
    return game != 0;
}

SHORT WINAPI HookedGetKeyState(int virtualKey) {
    if (!ForegroundBelongsToGame()) {
        return 0;
    }
    // Alt+Tab: the switcher takes the focus, but the game still glimpses the
    // TAB press and SA-MP toggles its scoreboard open. Alt+TAB never reaches
    // the game legitimately (the system always eats it), so TAB reads as
    // released while Alt is held.
    SHORT state = g_originalGetKeyState(virtualKey);
    if (virtualKey == VK_TAB) {
        bool altOrWin = ((g_originalGetKeyState(VK_MENU) |
                          g_originalGetKeyState(VK_LWIN) |
                          g_originalGetKeyState(VK_RWIN)) & 0x8000) != 0;
        bool suppress = altOrWin || TabRefocusGraceActive();
        if (state & 0x8000) {
            LogTabDownRead("GetKeyState", _ReturnAddress(), suppress);
        }
        if (suppress) {
            return 0;
        }
    }
    if (StickyMuteFilter(virtualKey, (state & 0x8000) != 0)) {
        return 0;
    }
    return state;
}

SHORT WINAPI HookedGetAsyncKeyState(int virtualKey) {
    if (!ForegroundBelongsToGame()) {
        return 0;
    }
    SHORT state = g_originalGetAsyncKeyState(virtualKey);
    if (virtualKey == VK_TAB) {
        bool suppress = AltOrWinHeldAsync() || TabRefocusGraceActive();
        if (state & 0x8000) {
            LogTabDownRead("GetAsyncKeyState", _ReturnAddress(), suppress);
        }
        if (suppress) {
            return 0;
        }
    }
    if (StickyMuteFilter(virtualKey, (state & 0x8000) != 0)) {
        return 0;
    }
    return state;
}

BOOL WINAPI HookedGetKeyboardState(PBYTE keyState) {
    BOOL result = g_originalGetKeyboardState(keyState);
    if (result && keyState) {
        __try {
            if (!ForegroundBelongsToGame()) {
                std::memset(keyState, 0, 256);
            } else {
                if (keyState[VK_TAB] & 0x80) {
                    bool altOrWin = ((keyState[VK_MENU] | keyState[VK_LMENU] |
                                      keyState[VK_RMENU] | keyState[VK_LWIN] |
                                      keyState[VK_RWIN]) & 0x80) != 0;
                    bool suppress = altOrWin || TabRefocusGraceActive();
                    LogTabDownRead("GetKeyboardState", _ReturnAddress(), suppress);
                    if (suppress) {
                        keyState[VK_TAB] = 0;
                    }
                }
                for (int key = 1; key < 256; ++key) {
                    if (g_stickyMutedKeys[key] &&
                        StickyMuteFilter(key, (keyState[key] & 0x80) != 0)) {
                        keyState[key] &= 0x7F;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return result;
}

}  // namespace

bool TabRefocusGraceActive() {
    DWORD last = g_lastRefocusTick;
    return last != 0 && (GetTickCount() - last) < kTabRefocusGraceMs;
}

bool AltOrWinHeldAsync() {
    if (!g_originalGetAsyncKeyState) {
        return false;
    }
    return ((g_originalGetAsyncKeyState(VK_MENU) |
             g_originalGetAsyncKeyState(VK_LWIN) |
             g_originalGetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
}

void MuteKeysHeldAtRefocus() {
    g_lastRefocusTick = GetTickCount();
    if (!g_originalGetAsyncKeyState) {
        return;
    }
    int muted = 0;
    for (int key = 1; key < 256; ++key) {
        if (g_originalGetAsyncKeyState(key) & 0x8000) {
            InterlockedExchange(&g_stickyMutedKeys[key], 1);
            InterlockedExchange(&g_msgMutedKeys[key], 1);
            ++muted;
        }
    }
    if (muted) {
        Log("refocus: muted %d held key(s) until released", muted);
    }
}

bool IsMessageKeyMuted(int virtualKey) {
    return g_msgMutedKeys[static_cast<unsigned>(virtualKey) & 0xFF] != 0;
}

void ClearMessageKeyMute(int virtualKey) {
    InterlockedExchange(&g_msgMutedKeys[static_cast<unsigned>(virtualKey) & 0xFF],
                        0);
}

void HookKeyStateApis() {
    if (GetConfig().disableInputFilters) {
        Log("input filters disabled by config");
        return;
    }
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    InstallExportHook(user32, "GetKeyState",
                      reinterpret_cast<void*>(&HookedGetKeyState),
                      reinterpret_cast<void**>(&g_originalGetKeyState));
    InstallExportHook(user32, "GetAsyncKeyState",
                      reinterpret_cast<void*>(&HookedGetAsyncKeyState),
                      reinterpret_cast<void**>(&g_originalGetAsyncKeyState));
    InstallExportHook(user32, "GetKeyboardState",
                      reinterpret_cast<void*>(&HookedGetKeyboardState),
                      reinterpret_cast<void**>(&g_originalGetKeyboardState));
}

}  // namespace bm
