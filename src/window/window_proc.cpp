#include "window/window_proc.h"

#include "core/config.h"
#include "core/log.h"
#include "d3d9/device_hooks.h"
#include "game/patches.h"
#include "input/key_filter.h"
#include "window/borderless.h"
#include "window/display_mode.h"

#include <cwchar>

namespace bm {
namespace {

WNDPROC g_previousWndProc = nullptr;
BOOL g_windowIsUnicode = FALSE;
volatile LONG g_fpsHotkeyPressConsumed = 0;

constexpr UINT_PTR kBackgroundRestoreTimerId = 0xB0DE1E56;
constexpr UINT kBackgroundRestoreDelayMs = 100;

// Retries the borderless geometry until the game has shown its own window.
constexpr UINT_PTR kBorderlessRetryTimerId = 0xB0DE1E57;
constexpr UINT kBorderlessRetryDelayMs = 250;
constexpr int kBorderlessRetryLimit = 40;  // ~10 s, then give up quietly
int g_borderlessRetries = 0;

LRESULT ForwardToGame(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_previousWndProc) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (g_windowIsUnicode) {
        return CallWindowProcW(g_previousWndProc, window, message, wParam, lParam);
    }
    return CallWindowProcA(g_previousWndProc, window, message, wParam, lParam);
}

void ScheduleBackgroundRestoreCheck(HWND window, const char* reason) {
    if (!window || !WindowPutAway()) {
        return;
    }
    SetTimer(window, kBackgroundRestoreTimerId, kBackgroundRestoreDelayMs,
             nullptr);
    Log("background restore check scheduled: %s iconic=%d visible=%d foreground=%d",
        reason, IsIconic(window) ? 1 : 0,
        IsWindowVisible(window) ? 1 : 0,
        GameOwnsForeground() ? 1 : 0);
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

void MinimizeForShowDesktop(HWND window, const char* reason) {
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
    Log("show desktop detected (%s): minimizing borderless window", reason);
    ShowWindow(window, SW_SHOWMINNOACTIVE);
}

// The plugin only observes these; the diagnostics they produce are what made
// the alt-tab and Win+D behaviour tractable in the first place.
void LogWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            Log("wnd WM_ACTIVATE window=0x%p wParam=0x%p lParam=0x%p active=%u minimized=%u",
                window, reinterpret_cast<void*>(wParam),
                reinterpret_cast<void*>(lParam),
                LOWORD(wParam), HIWORD(wParam));
            break;
        case WM_ACTIVATEAPP:
            Log("wnd WM_ACTIVATEAPP window=0x%p active=%u thread=%lu",
                window, static_cast<unsigned>(wParam),
                static_cast<unsigned long>(lParam));
            break;
        case WM_SETFOCUS:
            Log("wnd WM_SETFOCUS window=0x%p previous=0x%p borderless=%d",
                window, reinterpret_cast<void*>(wParam),
                BorderlessApplied() ? 1 : 0);
            break;
        case WM_KILLFOCUS:
            Log("wnd WM_KILLFOCUS window=0x%p next=0x%p",
                window, reinterpret_cast<void*>(wParam));
            break;
        case WM_SIZE:
            Log("wnd WM_SIZE window=0x%p type=%u size=%ux%u",
                window, static_cast<unsigned>(wParam),
                LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_DISPLAYCHANGE:
            Log("wnd WM_DISPLAYCHANGE window=0x%p bpp=%u size=%ux%u",
                window, static_cast<unsigned>(wParam),
                LOWORD(lParam), HIWORD(lParam));
            break;
        case WM_STYLECHANGING:
            Log("wnd WM_STYLECHANGING window=0x%p index=%ld borderless=%d",
                window, static_cast<long>(wParam),
                BorderlessApplied() ? 1 : 0);
            break;
        case WM_SHOWWINDOW:
            Log("wnd WM_SHOWWINDOW window=0x%p show=%u reason=%ld iconic=%d",
                window, static_cast<unsigned>(wParam),
                static_cast<long>(lParam),
                IsIconic(window) ? 1 : 0);
            break;
        case WM_SYSCOMMAND:
            Log("wnd WM_SYSCOMMAND window=0x%p command=0x%04X iconic=%d",
                window, static_cast<unsigned>(wParam & 0xFFF0),
                IsIconic(window) ? 1 : 0);
            break;
        // Shutdown messages. Without these the log simply stops when the game
        // goes away, which cannot be told apart from being killed outright.
        case WM_CLOSE:
            Log("wnd WM_CLOSE window=0x%p foreground=0x%p",
                window, GetForegroundWindow());
            break;
        case WM_DESTROY:
            Log("wnd WM_DESTROY window=0x%p", window);
            break;
        case WM_NCDESTROY:
            Log("wnd WM_NCDESTROY window=0x%p", window);
            break;
        case WM_QUIT:
            Log("wnd WM_QUIT window=0x%p exitCode=%u",
                window, static_cast<unsigned>(wParam));
            break;
        case WM_QUERYENDSESSION:
            Log("wnd WM_QUERYENDSESSION window=0x%p lParam=0x%p",
                window, reinterpret_cast<void*>(lParam));
            break;
        case WM_ENDSESSION:
            Log("wnd WM_ENDSESSION window=0x%p ending=%u",
                window, static_cast<unsigned>(wParam));
            break;
        case WM_WINDOWPOSCHANGING:
            // The single choke point every geometry, Z-order and visibility
            // change passes through, including changes started from another
            // process. Only the interesting ones are logged.
            if (lParam) {
                const WINDOWPOS* position =
                    reinterpret_cast<const WINDOWPOS*>(lParam);
                if ((position->flags & SWP_SHOWWINDOW) ||
                    !(position->flags & SWP_NOZORDER)) {
                    Log("wnd WM_WINDOWPOSCHANGING window=0x%p flags=0x%08X "
                        "insertAfter=0x%p pos=%d,%d size=%dx%d iconic=%d "
                        "foreground=%d",
                        window, position->flags, position->hwndInsertAfter,
                        position->x, position->y, position->cx, position->cy,
                        IsIconic(window) ? 1 : 0,
                        GameOwnsForeground() ? 1 : 0);
                }
            }
            break;
        default:
            break;
    }
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
                    ScheduleBackgroundRestoreCheck(window, "WM_ACTIVATE active");
                }
            } else {
                MinimizeForShowDesktop(window, "WM_ACTIVATE inactive");
            }
            break;
        case WM_ACTIVATEAPP:
            if (wParam) {
                if (WindowPutAway()) {
                    ScheduleBackgroundRestoreCheck(window,
                                                   "WM_ACTIVATEAPP active");
                }
            } else {
                MinimizeForShowDesktop(window, "WM_ACTIVATEAPP inactive");
            }
            break;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                SetWindowPutAway(true);
                SetBorderlessPending(true);
                KillTimer(window, kBackgroundRestoreTimerId);
                Log("window put away: minimized");
            } else if (WindowPutAway()) {
                // Do not trust SIZE_RESTORED alone. GTA can restore itself
                // behind the application the user selected. Give a genuine
                // taskbar/Alt+Tab activation a moment to arrive, then put the
                // game back if it still does not own the foreground.
                SetBorderlessPending(true);
                ScheduleBackgroundRestoreCheck(window, "WM_SIZE restored");
            } else {
                SetWindowPutAway(false);
                if (BorderlessPending()) {
                    ApplyBorderlessStyle(window);
                }
            }
            break;
        case WM_DISPLAYCHANGE:
            RequireNextReset("WM_DISPLAYCHANGE");
            RestoreDesktopMode("WM_DISPLAYCHANGE");
            break;
        case WM_SHOWWINDOW:
            if (!wParam && BorderlessApplied()) {
                SetWindowPutAway(true);
                SetBorderlessPending(true);
                KillTimer(window, kBackgroundRestoreTimerId);
                Log("window put away: hidden");
            } else if (wParam && WindowPutAway()) {
                ScheduleBackgroundRestoreCheck(window, "WM_SHOWWINDOW shown");
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

void HandleBorderlessRetryTimer(HWND window) {
    if (!BorderlessPending() || BorderlessApplied()) {
        KillTimer(window, kBorderlessRetryTimerId);
        return;
    }
    if (++g_borderlessRetries > kBorderlessRetryLimit) {
        KillTimer(window, kBorderlessRetryTimerId);
        Log("borderless retry gave up after %d attempts", g_borderlessRetries);
        return;
    }
    if (IsWindowVisible(window) && !IsIconic(window)) {
        KillTimer(window, kBorderlessRetryTimerId);
        Log("borderless retry: window is up after %d attempt(s)",
            g_borderlessRetries);
        ApplyBorderlessStyle(window);
    }
}

void HandleBackgroundRestoreTimer(HWND window) {
    KillTimer(window, kBackgroundRestoreTimerId);
    if (WindowPutAway() && !GameOwnsForeground()) {
        const bool iconic = IsIconic(window) != FALSE;
        const bool visible = IsWindowVisible(window) != FALSE;
        Log("background restore check fired: iconic=%d visible=%d",
            iconic ? 1 : 0, visible ? 1 : 0);
        if (!iconic) {
            // SW_SHOWMINNOACTIVE restores the minimized show state
            // without taking focus away from the user's window.
            ShowWindow(window, SW_SHOWMINNOACTIVE);
            Log("background restore cancelled: window minimized");
        }
        SetBorderlessPending(true);
        return;
    }

    SetWindowPutAway(false);
    Log("background restore accepted: game owns foreground");
    if (BorderlessPending() && !IsIconic(window)) {
        ApplyBorderlessStyle(window);
    }
}

// Returns true when the message was the FPS hotkey and must not reach the game.
bool HandleFpsHotkey(UINT message, WPARAM wParam, LPARAM lParam) {
    const Config& config = GetConfig();
    if (!config.fpsHotkeyEnabled || config.fpsHotkeyKey == 0 ||
        wParam != config.fpsHotkeyKey) {
        return false;
    }

    const bool keyUp = message == WM_KEYUP || message == WM_SYSKEYUP;
    if (keyUp) {
        return InterlockedExchange(&g_fpsHotkeyPressConsumed, 0) != 0;
    }

    const bool autoRepeat = (lParam & (1L << 30)) != 0;
    if (!autoRepeat) {
        const bool modifierDown =
            config.fpsHotkeyModifier == 0 ||
            (GetKeyState(static_cast<int>(config.fpsHotkeyModifier)) &
             0x8000) != 0;
        if (!modifierDown) {
            return false;
        }
        InterlockedExchange(&g_fpsHotkeyPressConsumed, 1);
        const bool enabled = ToggleFpsOverlay();
        Log("wnd FPS overlay toggled: enabled=%d modifier=0x%02X key=0x%02X",
            enabled ? 1 : 0, config.fpsHotkeyModifier, config.fpsHotkeyKey);
        return true;
    }

    // Autorepeat only belongs to us when the initial press was consumed.
    return InterlockedCompareExchange(&g_fpsHotkeyPressConsumed, 0, 0) != 0;
}

// Returns true when a key message must be hidden from the game.
bool SuppressKeyDown(UINT message, WPARAM wParam, LPARAM lParam) {
    if (wParam == VK_TAB) {
        Log("wnd TAB keydown message=%u lParam=0x%08lX",
            message, static_cast<unsigned long>(lParam));
        // The TAB press of Alt+Tab or Win+Tab; the system switches
        // focus anyway, but SA-MP would toggle its scoreboard open
        // before that happens.
        if ((message == WM_SYSKEYDOWN && (HIWORD(lParam) & KF_ALTDOWN)) ||
            ((GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) |
              GetKeyState(VK_RWIN)) & 0x8000)) {
            Log("wnd alt-tab TAB keydown suppressed message=%u", message);
            return true;
        }
        if (TabRefocusGraceActive()) {
            Log("wnd TAB keydown suppressed by refocus grace");
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
            Log("wnd keydown suppressed for held key vk=0x%02X message=%u",
                static_cast<unsigned>(wParam), message);
            return true;
        }
        ClearMessageKeyMute(virtualKey);
    }
    return false;
}

bool SuppressKeyUp(UINT message, WPARAM wParam) {
    if (wParam == VK_TAB) {
        Log("wnd TAB keyup message=%u", message);
        if (TabRefocusGraceActive()) {
            Log("wnd TAB keyup suppressed by refocus grace");
            return true;
        }
    }

    // The release of a muted key: the game never saw the press, so it must not
    // see the release either.
    const int virtualKey = static_cast<int>(wParam & 0xFF);
    if (IsMessageKeyMuted(virtualKey)) {
        ClearMessageKeyMute(virtualKey);
        Log("wnd keyup swallowed for held key vk=0x%02X message=%u",
            static_cast<unsigned>(wParam), message);
        return true;
    }
    return false;
}

LRESULT CALLBACK GameWndProc(HWND window, UINT message, WPARAM wParam,
                             LPARAM lParam) {
    LogWindowMessage(window, message, wParam, lParam);
    HandleShowStateMessage(window, message, wParam);

    switch (message) {
        case WM_SETFOCUS:
            // Avoid the vanilla restore-from-tray ESC menu. That menu is all
            // this arm is being suppressed for; the other thing GTA does here
            // is mark itself focused again, and that still has to happen.
            Log("wnd WM_SETFOCUS suppressed");
            RestoreGameInFocus("WM_SETFOCUS");
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
                RestoreGameInFocus("WM_ACTIVATE");
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
            break;

        case WM_NCDESTROY: {
            // GTA destroys and rebuilds its window while it settles on a video
            // mode. Let go of the subclass here, otherwise the next window is
            // never hooked and g_previousWndProc points at a dead chain.
            LRESULT result = ForwardToGame(window, message, wParam, lParam);
            KillTimer(window, kBackgroundRestoreTimerId);
            KillTimer(window, kBorderlessRetryTimerId);
            g_previousWndProc = nullptr;
            g_borderlessRetries = 0;
            ResetBorderlessState();
            Log("window hook released: window=0x%p", window);
            return result;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (HandleFpsHotkey(message, wParam, lParam)) {
                return 0;
            }
            if (SuppressKeyDown(message, wParam, lParam)) {
                return 0;
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (HandleFpsHotkey(message, wParam, lParam)) {
                return 0;
            }
            if (SuppressKeyUp(message, wParam)) {
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
                    Log("wnd style before=0x%08lX", style->styleNew);
                    style->styleNew &= ~static_cast<DWORD>(kFrameStyleBits);
                    style->styleNew |= WS_POPUP;
                    Log("wnd style after=0x%08lX", style->styleNew);
                } else if (wParam == static_cast<WPARAM>(GWL_EXSTYLE)) {
                    Log("wnd exstyle before=0x%08lX", style->styleNew);
                    style->styleNew &= ~static_cast<DWORD>(kFrameExStyleBits);
                    Log("wnd exstyle after=0x%08lX", style->styleNew);
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
    Log("borderless retry armed: window=0x%p visible=%d iconic=%d",
        window, IsWindowVisible(window) ? 1 : 0, IsIconic(window) ? 1 : 0);
}

void InstallWindowHook(HWND window) {
    if (GetConfig().disableWindowHook) {
        Log("window hook disabled by config");
        return;
    }
    if (g_previousWndProc || !window || !IsWindow(window)) {
        Log("window hook skipped: existing=%d window=0x%p isWindow=%d",
            g_previousWndProc ? 1 : 0, window,
            window ? (IsWindow(window) ? 1 : 0) : 0);
        return;
    }

    g_windowIsUnicode = IsWindowUnicode(window);

    LONG_PTR hook = reinterpret_cast<LONG_PTR>(&GameWndProc);
    LONG_PTR previous = g_windowIsUnicode
        ? SetWindowLongPtrW(window, GWLP_WNDPROC, hook)
        : SetWindowLongPtrA(window, GWLP_WNDPROC, hook);

    if (previous) {
        g_previousWndProc = reinterpret_cast<WNDPROC>(previous);
        Log("window hook installed: window=0x%p unicode=%d previous=0x%p",
            window, g_windowIsUnicode ? 1 : 0,
            reinterpret_cast<void*>(g_previousWndProc));
    } else {
        Log("window hook failed: window=0x%p error=%lu",
            window, GetLastError());
    }
}

}  // namespace bm
