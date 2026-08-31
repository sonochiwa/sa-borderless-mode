#include "window/borderless.h"

#include "core/log.h"

namespace bm {
namespace {

bool g_borderlessApplied = false;
bool g_borderlessPending = false;
bool g_windowPutAway = false;

using SetThreadDpiContextFn = HANDLE (WINAPI*)(HANDLE);

// The monitor rectangle must be read in physical pixels, whatever DPI
// awareness the game process declared.
class ScopedDpiAwareness {
public:
    ScopedDpiAwareness() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) {
            return;
        }
        set_ = reinterpret_cast<SetThreadDpiContextFn>(
            GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (set_) {
            previous_ = set_(reinterpret_cast<HANDLE>(-4));
            if (!previous_) {
                previous_ = set_(reinterpret_cast<HANDLE>(-2));
            }
        }
    }

    ~ScopedDpiAwareness() {
        if (set_ && previous_) {
            set_(previous_);
        }
    }

    ScopedDpiAwareness(const ScopedDpiAwareness&) = delete;
    ScopedDpiAwareness& operator=(const ScopedDpiAwareness&) = delete;

private:
    SetThreadDpiContextFn set_ = nullptr;
    HANDLE previous_ = nullptr;
};

// Takes the foreground the first time the borderless window is shown, working
// around the foreground lock by borrowing the current owner's input queue.
void TakeForeground(HWND window) {
    HWND foreground = GetForegroundWindow();
    const DWORD ourThread = GetCurrentThreadId();
    DWORD foregroundThread = 0;
    if (foreground && foreground != window) {
        foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
    }
    if (foregroundThread && foregroundThread != ourThread) {
        AttachThreadInput(foregroundThread, ourThread, TRUE);
        SetForegroundWindow(window);
        AttachThreadInput(foregroundThread, ourThread, FALSE);
    } else {
        SetForegroundWindow(window);
    }
    BringWindowToTop(window);
    SetFocus(window);
    Log("borderless foreground/focus requested: foregroundBefore=0x%p",
        foreground);
}

}  // namespace

bool GameOwnsForeground() {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &pid);
    }
    return pid == GetCurrentProcessId();
}

bool BorderlessApplied() {
    return g_borderlessApplied;
}

bool BorderlessPending() {
    return g_borderlessPending;
}

void SetBorderlessPending(bool pending) {
    g_borderlessPending = pending;
}

bool WindowPutAway() {
    return g_windowPutAway;
}

void SetWindowPutAway(bool putAway) {
    g_windowPutAway = putAway;
}

void ApplyBorderlessStyle(HWND window) {
    if (!window || !IsWindow(window)) {
        Log("borderless skipped: invalid window=0x%p", window);
        return;
    }

    // Minimized or hidden (Win+D, taskbar, "show desktop"): re-applying the
    // borderless geometry here would pull the window back up behind whatever
    // the user switched to. The game keeps calling Reset while minimized, so
    // this path is hit as soon as anything re-arms the forwarded Reset. Wait
    // for the window to come back instead. The very first application is
    // exempt: that one has to show the window.
    const bool firstApply = !g_borderlessApplied;
    const bool iconic = IsIconic(window) != FALSE;
    const bool visible = IsWindowVisible(window) != FALSE;
    if (iconic || (!firstApply && !visible)) {
        g_borderlessPending = true;
        Log("borderless deferred: window=0x%p iconic=%d visible=%d first=%d",
            window, iconic ? 1 : 0, visible ? 1 : 0, firstApply ? 1 : 0);
        return;
    }
    g_borderlessPending = false;

    ScopedDpiAwareness dpiAware;

    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);

    HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(handle, &monitor)) {
        Log("borderless skipped: GetMonitorInfo failed window=0x%p monitor=0x%p error=%lu",
            window, handle, GetLastError());
        return;
    }

    LONG style = GetWindowLongW(window, GWL_STYLE);
    LONG oldStyle = style;
    style &= ~kFrameStyleBits;
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongW(window, GWL_STYLE, style);

    LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);
    LONG oldExStyle = exStyle;
    exStyle &= ~kFrameExStyleBits;
    SetWindowLongW(window, GWL_EXSTYLE, exStyle);

    int width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    int height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;

    // Only claim the top of the Z-order while the game is the foreground app.
    // A Reset that happens while the user works in another window must not
    // raise the game over it, and must never activate it.
    const bool gameForeground = GameOwnsForeground();
    UINT positionFlags = SWP_FRAMECHANGED | SWP_NOACTIVATE;
    if (!gameForeground) {
        positionFlags |= SWP_NOZORDER;
    }
    if (firstApply && !visible) {
        positionFlags |= SWP_SHOWWINDOW;
    }

    Log("borderless applying: window=0x%p monitor=(%ld,%ld)-(%ld,%ld) "
        "size=%dx%d style=0x%08lX->0x%08lX exstyle=0x%08lX->0x%08lX first=%d "
        "foreground=%d flags=0x%08X",
        window,
        monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right, monitor.rcMonitor.bottom,
        width, height,
        oldStyle, style, oldExStyle, exStyle,
        firstApply ? 1 : 0,
        gameForeground ? 1 : 0, positionFlags);

    SetWindowPos(window, gameForeground ? HWND_TOP : nullptr,
                 monitor.rcMonitor.left, monitor.rcMonitor.top,
                 width, height,
                 positionFlags);

    if (firstApply) {
        TakeForeground(window);
    }

    g_borderlessApplied = true;
    Log("borderless applied");
}

}  // namespace bm
