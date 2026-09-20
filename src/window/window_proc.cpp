#include "window/window_proc.h"

#include "d3d9/device_hooks.h"
#include "game/patches.h"
#include "input/key_filter.h"
#include "window/borderless.h"
#include "window/display_mode.h"

#include <cwchar>

namespace bm {
namespace {

WNDPROC g_previousWndProc = nullptr;
HWND g_hookedWindow = nullptr;
BOOL g_windowIsUnicode = FALSE;

constexpr UINT_PTR kBackgroundRestoreTimerId = 0xB0DE1E56;
constexpr UINT kBackgroundRestoreDelayMs = 100;

// Retries the borderless geometry until the game has shown its own window.
constexpr UINT_PTR kBorderlessRetryTimerId = 0xB0DE1E57;
constexpr UINT kBorderlessRetryDelayMs = 250;
// About ten seconds, then give up quietly.
constexpr int kBorderlessRetryLimit = 40;
int g_borderlessRetries = 0;

// Presents the last frame again while the game idles in the background. GTA
// pumps its message queue every 100 ms in that state and nothing else, so a
// window timer is the one thing that still runs on the game thread. Half a
// second keeps well inside the three seconds after which the NVIDIA App
// overlay gives up on a borderless window that stopped presenting.
constexpr UINT_PTR kIdlePresentTimerId = 0xB0DE1E58;
constexpr UINT kIdlePresentIntervalMs = 500;

LRESULT ForwardToGame(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_previousWndProc) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (g_windowIsUnicode) {
        return CallWindowProcW(g_previousWndProc, window, message, wParam, lParam);
    }
    return CallWindowProcA(g_previousWndProc, window, message, wParam, lParam);
}

void ScheduleBackgroundRestoreCheck(HWND window) {
    if (!window || !WindowPutAway()) {
        return;
    }
    SetTimer(window, kBackgroundRestoreTimerId, kBackgroundRestoreDelayMs,
             nullptr);
}

bool ShowDesktopOwnsForeground() {
    HWND foreground = GetForegroundWindow();
    HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;
    if (root) {
        foreground = root;
    }

    wchar_t className[32] = {};
    if (!foreground ||
        !GetClassNameW(foreground, className,
                       static_cast<int>(sizeof(className) /
                                        sizeof(className[0])))) {
        return false;
    }
    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0;
}

void MinimizeForShowDesktop(HWND window) {
    if (!BorderlessApplied() || !window || IsIconic(window) ||
        !ShowDesktopOwnsForeground()) {
        return;
    }

    // A WS_POPUP borderless window is only placed behind Progman by Win+D;
    // Explorer does not give it the normal minimized show state. When Show
    // Desktop ends because another application opens, that leaves GTA visible
    // behind it. Explicitly give the game the minimized state while Progman is
    // foreground, without taking activation away from the shell.
    SetWindowPutAway(true);
    SetBorderlessPending(true);
    ShowWindow(window, SW_SHOWMINNOACTIVE);
}

// Tracks minimize/restore and the "restored behind another application" case.
void HandleShowStateMessage(HWND window, UINT message, WPARAM wParam) {
    switch (message) {
        case WM_ACTIVATE:
            // A windowed D3D9 device does not need to be reset merely because
            // it lost activation. Re-arming GTA's redundant Reset here was the
            // first half of the background-restore bug.
            if (LOWORD(wParam) != WA_INACTIVE) {
                if (WindowPutAway()) {
                    // GTA can activate itself momentarily while another app is
                    // opening. Accept the restore only if it still owns the
                    // foreground after the short verification interval.
                    ScheduleBackgroundRestoreCheck(window);
                }
            } else {
                MinimizeForShowDesktop(window);
            }
            break;
        case WM_ACTIVATEAPP:
            if (wParam) {
                if (WindowPutAway()) {
                    ScheduleBackgroundRestoreCheck(window);
                }
            } else {
                MinimizeForShowDesktop(window);
            }
            break;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                SetWindowPutAway(true);
                SetBorderlessPending(true);
                KillTimer(window, kBackgroundRestoreTimerId);
            } else if (WindowPutAway()) {
                // Do not trust SIZE_RESTORED alone. GTA can restore itself
                // behind the application the user selected. Give a genuine
                // taskbar/Alt+Tab activation a moment to arrive, then put the
                // game back if it still does not own the foreground.
                SetBorderlessPending(true);
                ScheduleBackgroundRestoreCheck(window);
            } else {
                SetWindowPutAway(false);
                if (BorderlessPending()) {
                    ApplyBorderlessStyle(window);
                }
            }
            break;
        case WM_DISPLAYCHANGE:
            RequireNextReset();
            RestoreDesktopMode();
            break;
        case WM_SHOWWINDOW:
            if (!wParam && BorderlessApplied()) {
                SetWindowPutAway(true);
                SetBorderlessPending(true);
                KillTimer(window, kBackgroundRestoreTimerId);
            } else if (wParam && WindowPutAway()) {
                ScheduleBackgroundRestoreCheck(window);
            } else if (wParam && BorderlessPending()) {
                // The game is showing its window for the first time; the
                // geometry deferred during CreateDevice can go on now.
                ScheduleBorderlessRetry(window);
            }
            break;
        default:
            break;
    }
}

// True while the game is sitting in its unfocused idle loop instead of
// rendering. On the supported executable that is exactly GTA's own flag; on
// any other, the foreground application decides, which is what GTA bases the
// flag on anyway.
bool GameIdling() {
    const int inFocus = ReadGameInFocusFlag();
    if (inFocus >= 0) {
        return inFocus == 0;
    }
    return !GameOwnsForeground();
}

void HandleIdlePresentTimer() {
    if (GameIdling()) {
        PresentIdleFrame();
    }
}

void HandleBorderlessRetryTimer(HWND window) {
    if (!BorderlessPending() || BorderlessApplied()) {
        KillTimer(window, kBorderlessRetryTimerId);
        return;
    }
    if (++g_borderlessRetries > kBorderlessRetryLimit) {
        KillTimer(window, kBorderlessRetryTimerId);
        return;
    }
    if (IsWindowVisible(window) && !IsIconic(window)) {
        KillTimer(window, kBorderlessRetryTimerId);
        ApplyBorderlessStyle(window);
    }
}

void HandleBackgroundRestoreTimer(HWND window) {
    KillTimer(window, kBackgroundRestoreTimerId);
    if (WindowPutAway() && !GameOwnsForeground()) {
        const bool iconic = IsIconic(window) != FALSE;
        if (!iconic) {
            // SW_SHOWMINNOACTIVE restores the minimized show state
            // without taking focus away from the user's window.
            ShowWindow(window, SW_SHOWMINNOACTIVE);
        }
        SetBorderlessPending(true);
        return;
    }

    SetWindowPutAway(false);
    if (BorderlessPending() && !IsIconic(window)) {
        ApplyBorderlessStyle(window);
    }
}

// Returns true when a key message must be hidden from the game.
bool SuppressKeyDown(UINT message, WPARAM wParam, LPARAM lParam) {
    if (wParam == VK_TAB) {
        // The TAB press of Alt+Tab or Win+Tab; the system switches
        // focus anyway, but SA-MP would toggle its scoreboard open
        // before that happens.
        if ((message == WM_SYSKEYDOWN && (HIWORD(lParam) & KF_ALTDOWN)) ||
            ((GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) |
              GetKeyState(VK_RWIN)) & 0x8000)) {
            return true;
        }
        if (TabRefocusGraceActive()) {
            return true;
        }
    }

    // A keydown of a key still held from before the refocus (e.g. TAB
    // released after Alt when tabbing back in) must not reach the game as a
    // fresh press. Queued autorepeats can be processed after the physical
    // release, so the mute lifts only on this window's own WM_KEYUP or on a
    // keydown without the repeat bit (KF_REPEAT clear = genuinely new press).
    const int virtualKey = static_cast<int>(wParam & 0xFF);
    if (IsMessageKeyMuted(virtualKey)) {
        if (HIWORD(lParam) & KF_REPEAT) {
            return true;
        }
        ClearMessageKeyMute(virtualKey);
    }
    return false;
}

bool SuppressKeyUp(WPARAM wParam) {
    if (wParam == VK_TAB) {
        if (TabRefocusGraceActive()) {
            return true;
        }
    }

    // The release of a muted key: the game never saw the press, so it must not
    // see the release either.
    const int virtualKey = static_cast<int>(wParam & 0xFF);
    if (IsMessageKeyMuted(virtualKey)) {
        ClearMessageKeyMute(virtualKey);
        return true;
    }
    return false;
}

LRESULT CALLBACK GameWndProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    HandleShowStateMessage(window, message, wParam);

    switch (message) {
        case WM_SETFOCUS:
            // Avoid the vanilla restore-from-tray ESC menu. That menu is all
            // this arm is being suppressed for; the other thing GTA does here
            // is mark itself focused again, and that still has to happen.
            RestoreGameInFocus();
            MuteKeysHeldAtRefocus();
            return 0;

        case WM_ACTIVATE:
            // Snapshot held keys as early as possible on focus gain, before
            // the game or SA-MP polls or receives autorepeat messages.
            if (LOWORD(wParam) != WA_INACTIVE) {
                // The game's own write here is a NOP once SAMPGraphicRestore
                // has loaded, and the WM_SETFOCUS arm never runs because this
                // plugin swallows the message. Without this the flag stays
                // clear and WinMain idles at 100 ms a frame for good.
                RestoreGameInFocus();
                MuteKeysHeldAtRefocus();
                if (!WindowPutAway() && BorderlessPending() &&
                    !IsIconic(window)) {
                    ApplyBorderlessStyle(window);
                }
            }
            break;

        case WM_TIMER:
            if (wParam == kBackgroundRestoreTimerId) {
                HandleBackgroundRestoreTimer(window);
                return 0;
            }
            if (wParam == kBorderlessRetryTimerId) {
                HandleBorderlessRetryTimer(window);
                return 0;
            }
            if (wParam == kIdlePresentTimerId) {
                HandleIdlePresentTimer();
                return 0;
            }
            break;

        case WM_NCDESTROY: {
            // GTA destroys and rebuilds its window while it settles on a video
            // mode. Let go of the subclass here, otherwise the next window is
            // never hooked and g_previousWndProc points at a dead chain.
            LRESULT result = ForwardToGame(window, message, wParam, lParam);
            KillTimer(window, kBackgroundRestoreTimerId);
            KillTimer(window, kBorderlessRetryTimerId);
            KillTimer(window, kIdlePresentTimerId);
            ReleaseIdleDevice();
            g_previousWndProc = nullptr;
            g_hookedWindow = nullptr;
            g_borderlessRetries = 0;
            ResetBorderlessState();
            return result;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (SuppressKeyDown(message, wParam, lParam)) {
                return 0;
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (SuppressKeyUp(wParam)) {
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (BorderlessApplied() &&
                LOWORD(lParam) == HTCLIENT && GetForegroundWindow() == window) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;

        case WM_STYLECHANGING:
            // Keep the game/RenderWare from restoring window borders.
            if (BorderlessApplied() && lParam) {
                STYLESTRUCT* style = reinterpret_cast<STYLESTRUCT*>(lParam);
                if (wParam == static_cast<WPARAM>(GWL_STYLE)) {
                    style->styleNew &= ~static_cast<DWORD>(kFrameStyleBits);
                    style->styleNew |= WS_POPUP;
                } else if (wParam == static_cast<WPARAM>(GWL_EXSTYLE)) {
                    style->styleNew &= ~static_cast<DWORD>(kFrameExStyleBits);
                }
            }
            break;

        default:
            break;
    }

    return ForwardToGame(window, message, wParam, lParam);
}

}  // namespace

void ScheduleBorderlessRetry(HWND window) {
    if (!window || !IsWindow(window) || !BorderlessPending()) {
        return;
    }
    g_borderlessRetries = 0;
    SetTimer(window, kBorderlessRetryTimerId, kBorderlessRetryDelayMs, nullptr);
}

void InstallWindowHook(HWND window) {
    if (g_previousWndProc || !window || !IsWindow(window)) {
        return;
    }

    g_windowIsUnicode = IsWindowUnicode(window);

    LONG_PTR hook = reinterpret_cast<LONG_PTR>(&GameWndProc);
    LONG_PTR previous = g_windowIsUnicode
        ? SetWindowLongPtrW(window, GWLP_WNDPROC, hook)
        : SetWindowLongPtrA(window, GWLP_WNDPROC, hook);

    if (previous) {
        g_previousWndProc = reinterpret_cast<WNDPROC>(previous);
        g_hookedWindow = window;
        // Armed for the life of the window; the handler does nothing while
        // the game renders on its own.
        SetTimer(window, kIdlePresentTimerId, kIdlePresentIntervalMs, nullptr);
    } else {
    }
}

HWND HookedWindow() {
    return g_hookedWindow;
}

}  // namespace bm
