#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "MinHook.h"

namespace {

constexpr int kVtableCreateDevice = 16;
constexpr int kVtableReset = 16;

const wchar_t kIniSection[] = L"BorderlessMode";
const wchar_t kLegacyIniSection[] = L"SABorderless";

// UTF-16 LE with BOM: the profile APIs read it natively, and text editors
// stop misdetecting the short file as UTF-16 mojibake ("Chinese" text).
const wchar_t kDefaultIni[] =
    L"\xFEFF"
    L"[BorderlessMode]\r\n"
    L"AntiAFK=0\r\n"
    L"Log=1\r\n";

enum ConvertMode {
    ConvertNone = 0,
    ConvertFullscreen,
    ConvertVsyncOnly,
};

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT sdkVersion);
using CreateDeviceFn = HRESULT (WINAPI*)(IDirect3D9* self, UINT adapter,
                                         D3DDEVTYPE deviceType, HWND focusWindow,
                                         DWORD behaviorFlags,
                                         D3DPRESENT_PARAMETERS* params,
                                         IDirect3DDevice9** device);
using ResetFn = HRESULT (WINAPI*)(IDirect3DDevice9* self,
                                  D3DPRESENT_PARAMETERS* params);

HMODULE g_module = nullptr;
bool g_antiAfk = false;
bool g_logEnabled = true;

HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
bool g_logLockInitialized = false;

void Log(const char* format, ...);

bool BuildSiblingPath(const wchar_t* extension, wchar_t* path, DWORD pathSize) {
    DWORD length = GetModuleFileNameW(g_module, path, pathSize);
    if (length == 0 || length >= pathSize) {
        return false;
    }

    wchar_t* dot = wcsrchr(path, L'.');
    if (!dot) {
        return false;
    }

    wcscpy_s(dot, pathSize - (dot - path), extension);
    return true;
}

void LogOpen() {
    if (!g_logEnabled || g_logFile != INVALID_HANDLE_VALUE) {
        return;
    }

    wchar_t logPath[MAX_PATH] = {};
    if (!BuildSiblingPath(L".log", logPath, MAX_PATH)) {
        return;
    }
    g_logFile = CreateFileW(logPath, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        Log("log opened: %ls", logPath);
    }
}

void LogClose() {
    if (g_logFile != INVALID_HANDLE_VALUE) {
        Log("log closing");
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
}

void Log(const char* format, ...) {
    if (g_logFile == INVALID_HANDLE_VALUE || !g_logLockInitialized) {
        return;
    }

    char message[1024] = {};
    int prefix = std::snprintf(message, sizeof(message), "[%8u ms|tid %5u] ",
                               GetTickCount(), GetCurrentThreadId());
    if (prefix < 0 || prefix >= static_cast<int>(sizeof(message))) {
        return;
    }

    va_list args;
    va_start(args, format);
    int body = std::vsnprintf(message + prefix, sizeof(message) - prefix - 3,
                              format, args);
    va_end(args);

    int total = prefix + (body > 0 ? body : 0);
    if (total > static_cast<int>(sizeof(message)) - 3) {
        total = static_cast<int>(sizeof(message)) - 3;
    }
    message[total++] = '\r';
    message[total++] = '\n';

    EnterCriticalSection(&g_logLock);
    DWORD written = 0;
    WriteFile(g_logFile, message, total, &written, nullptr);
    FlushFileBuffers(g_logFile);
    LeaveCriticalSection(&g_logLock);
}

const char* ConvertModeName(ConvertMode mode) {
    switch (mode) {
        case ConvertFullscreen:
            return "fullscreen-to-borderless";
        case ConvertVsyncOnly:
            return "vsync-only";
        default:
            return "none";
    }
}

void LogPresentParams(const char* label, const D3DPRESENT_PARAMETERS* params) {
    if (g_logFile == INVALID_HANDLE_VALUE) {
        return;
    }

    __try {
        if (!params) {
            Log("%s params=null", label);
            return;
        }

        Log("%s Windowed=%u BackBuffer=%ux%u Format=%u Count=%u "
            "SwapEffect=%u hDeviceWindow=0x%p AutoDepth=%u DepthFormat=%u "
            "Refresh=%u Interval=%u Flags=0x%08X",
            label,
            params->Windowed,
            params->BackBufferWidth,
            params->BackBufferHeight,
            params->BackBufferFormat,
            params->BackBufferCount,
            params->SwapEffect,
            params->hDeviceWindow,
            params->EnableAutoDepthStencil,
            params->AutoDepthStencilFormat,
            params->FullScreen_RefreshRateInHz,
            params->PresentationInterval,
            params->Flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("%s params=<exception while reading>", label);
    }
}

WNDPROC g_previousWndProc = nullptr;
BOOL g_windowIsUnicode = FALSE;

using SetCursorPosFn = BOOL (WINAPI*)(int x, int y);

Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
ResetFn g_originalReset = nullptr;
SetCursorPosFn g_originalSetCursorPos = nullptr;

LONG g_createDeviceHookState = 0;

D3DPRESENT_PARAMETERS g_appliedParams = {};
bool g_haveAppliedParams = false;
bool g_haveForwardedReset = false;
int g_suppressedRedundantResetCount = 0;

bool SameDisplayMode(const D3DPRESENT_PARAMETERS& a, const D3DPRESENT_PARAMETERS& b) {
    return a.Windowed == b.Windowed &&
           a.BackBufferWidth == b.BackBufferWidth &&
           a.BackBufferHeight == b.BackBufferHeight &&
           a.BackBufferFormat == b.BackBufferFormat &&
           a.BackBufferCount == b.BackBufferCount &&
           a.MultiSampleType == b.MultiSampleType &&
           a.SwapEffect == b.SwapEffect &&
           a.hDeviceWindow == b.hDeviceWindow &&
           a.EnableAutoDepthStencil == b.EnableAutoDepthStencil &&
           a.AutoDepthStencilFormat == b.AutoDepthStencilFormat &&
           a.FullScreen_RefreshRateInHz == b.FullScreen_RefreshRateInHz &&
           a.PresentationInterval == b.PresentationInterval;
}

void RememberAppliedParams(const D3DPRESENT_PARAMETERS* params) {
    __try {
        if (params) {
            g_appliedParams = *params;
            g_haveAppliedParams = true;
            LogPresentParams("remembered", params);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("remember params failed with exception");
    }
}

bool IsRedundantReset(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* params) {
    if (!device || !params || !g_haveAppliedParams) {
        return false;
    }
    __try {
        HRESULT cooperative = device->TestCooperativeLevel();
        bool redundant = SameDisplayMode(*params, g_appliedParams) &&
                         cooperative == D3D_OK;
        return redundant;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("redundant reset check failed with exception");
        return false;
    }
}

void RequireNextReset(const char* reason) {
    g_haveForwardedReset = false;
    Log("next Reset will be forwarded: %s", reason);
}

bool BuildIniPath(wchar_t* path, DWORD pathSize) {
    return BuildSiblingPath(L".ini", path, pathSize);
}

void CreateDefaultIniIfMissing(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, kDefaultIni, sizeof(kDefaultIni) - sizeof(wchar_t), &written,
              nullptr);
    CloseHandle(file);
}

void LoadConfig() {
    wchar_t iniPath[MAX_PATH] = {};
    if (!BuildIniPath(iniPath, MAX_PATH)) {
        return;
    }

    CreateDefaultIniIfMissing(iniPath);

    int antiAfk = GetPrivateProfileIntW(kIniSection, L"AntiAFK", -1, iniPath);
    if (antiAfk < 0) {
        antiAfk = GetPrivateProfileIntW(kLegacyIniSection, L"AntiAFK", 0, iniPath);
    }

    g_antiAfk = antiAfk != 0;
    g_logEnabled = GetPrivateProfileIntW(kIniSection, L"Log", 1, iniPath) != 0;

    Log("config loaded: ini=%ls Log=%d AntiAFK=%d", iniPath,
        g_logEnabled ? 1 : 0, g_antiAfk ? 1 : 0);
}

bool g_borderlessApplied = false;

DEVMODEW g_desktopMode = {};
bool g_haveDesktopMode = false;

// Defined next to the ChangeDisplaySettings hooks below.
void RestoreDesktopMode(const char* reason);

// Keys that are still physically held at the moment the game regains the
// foreground (the TAB of an Alt+Tab released a moment after Alt, a mouse
// button used to click the window, keys typed in another app) must not leak
// in as fresh presses. Polled APIs and window messages each keep their own
// mute table, because they observe the release at different times: queued
// autorepeat WM_KEYDOWNs and the lagging GetKeyState table can still report
// the key long after the physical (async) state says it was released.
volatile LONG g_stickyMutedKeys[256] = {};  // polled key-state APIs
volatile LONG g_msgMutedKeys[256] = {};     // window keyboard messages

// TAB passes through with zero delay. In a real-world Alt+Tab the whole TAB
// tap can land in the game before Alt even engages. Refocus must only mute
// delayed/queued TAB state; injecting another TAB on restore can open the
// SA-MP scoreboard after the original press was already hidden.
volatile DWORD g_lastRealTabDownTick = 0;
volatile LONG g_tabCompensatePending = 0;

constexpr UINT_PTR kTabDeliveryTimerId = 0xB0DE1E55;
constexpr DWORD kTabUndoWindowMs = 450;
constexpr UINT kTabCompensateDelayMs = 300;
constexpr UINT kTabSynthTapMs = 60;

// 0 = idle, 1 = compensation scheduled, 2 = injected keydown out, keyup
// pending.
volatile LONG g_tabDeliveryPhase = 0;

// The injected tap goes through SendInput so it reaches SA-MP's pump-level
// hook naturally; while it is in flight the filters in this file must let
// TAB through.
volatile DWORD g_tabSynthStartTick = 0;

bool TabSynthActive() {
    DWORD start = g_tabSynthStartTick;
    return start != 0 && (GetTickCount() - start) < 700;
}

void InjectTabEvent(bool up) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = VK_TAB;
    input.ki.wScan = 0x0F;
    input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    UINT sent = SendInput(1, &input, sizeof(input));
    Log("TAB inject %s: sent=%u", up ? "keyup" : "keydown", sent);
}

// Defined next to the key-state hooks below.
void MuteKeysHeldAtRefocus();
bool TabRefocusGraceActive();
void LogTabDownRead(const char* api, void* caller, bool suppressed);
void InstallGetMessageHook(HWND window);

const LONG kFrameStyleBits = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                             WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME;
const LONG kFrameExStyleBits = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                               WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

LRESULT ForwardToGame(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (!g_previousWndProc) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    if (g_windowIsUnicode) {
        return CallWindowProcW(g_previousWndProc, window, message, wParam, lParam);
    }
    return CallWindowProcA(g_previousWndProc, window, message, wParam, lParam);
}

void NoteFocusLossForTab(HWND window, const char* reason) {
    LONG phase = InterlockedExchange(&g_tabDeliveryPhase, 0);
    if (phase != 0) {
        KillTimer(window, kTabDeliveryTimerId);
        if (phase == 2) {
            // The injected keydown already went out; close the pair so
            // nothing is left with a stuck TAB.
            InjectTabEvent(true);
        }
    }
    InterlockedExchange(&g_tabCompensatePending, 0);
    g_lastRealTabDownTick = 0;
    Log("wnd focus lost (%s): TAB compensation cleared", reason);
}

void StartTabCompensationIfPending(HWND window) {
    if (!InterlockedExchange(&g_tabCompensatePending, 0)) {
        return;
    }
    InterlockedExchange(&g_tabDeliveryPhase, 1);
    SetTimer(window, kTabDeliveryTimerId, kTabCompensateDelayMs, nullptr);
    Log("wnd TAB undo timer started");
}

LRESULT CALLBACK GameWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ACTIVATE:
            Log("wnd WM_ACTIVATE window=0x%p wParam=0x%p lParam=0x%p active=%u minimized=%u antiAfk=%d",
                window, reinterpret_cast<void*>(wParam), reinterpret_cast<void*>(lParam),
                LOWORD(wParam), HIWORD(wParam), g_antiAfk ? 1 : 0);
            RequireNextReset("WM_ACTIVATE");
            break;
        case WM_ACTIVATEAPP:
            Log("wnd WM_ACTIVATEAPP window=0x%p active=%u thread=%lu antiAfk=%d",
                window, static_cast<unsigned>(wParam), static_cast<unsigned long>(lParam),
                g_antiAfk ? 1 : 0);
            RequireNextReset("WM_ACTIVATEAPP");
            break;
        case WM_SETFOCUS:
            Log("wnd WM_SETFOCUS window=0x%p previous=0x%p borderless=%d",
                window, reinterpret_cast<void*>(wParam), g_borderlessApplied ? 1 : 0);
            break;
        case WM_KILLFOCUS:
            Log("wnd WM_KILLFOCUS window=0x%p next=0x%p antiAfk=%d",
                window, reinterpret_cast<void*>(wParam), g_antiAfk ? 1 : 0);
            break;
        case WM_SIZE:
            Log("wnd WM_SIZE window=0x%p type=%u size=%ux%u",
                window, static_cast<unsigned>(wParam),
                LOWORD(lParam), HIWORD(lParam));
            RequireNextReset("WM_SIZE");
            break;
        case WM_DISPLAYCHANGE:
            Log("wnd WM_DISPLAYCHANGE window=0x%p bpp=%u size=%ux%u",
                window, static_cast<unsigned>(wParam),
                LOWORD(lParam), HIWORD(lParam));
            RequireNextReset("WM_DISPLAYCHANGE");
            RestoreDesktopMode("WM_DISPLAYCHANGE");
            break;
        case WM_STYLECHANGING:
            Log("wnd WM_STYLECHANGING window=0x%p index=%ld borderless=%d",
                window, static_cast<long>(wParam), g_borderlessApplied ? 1 : 0);
            break;
    }

    switch (message) {
        case WM_SETFOCUS:
            // Avoid the vanilla restore-from-tray ESC menu.
            Log("wnd WM_SETFOCUS suppressed");
            MuteKeysHeldAtRefocus();
            StartTabCompensationIfPending(window);
            return 0;

        case WM_ACTIVATE:
            // Snapshot held keys as early as possible on focus gain, before
            // the game or SA-MP polls or receives autorepeat messages.
            if (LOWORD(wParam) != WA_INACTIVE) {
                MuteKeysHeldAtRefocus();
                StartTabCompensationIfPending(window);
            } else {
                NoteFocusLossForTab(window, "WM_ACTIVATE inactive");
            }
            break;

        case WM_KILLFOCUS:
            NoteFocusLossForTab(window, "WM_KILLFOCUS");
            break;

        case WM_TIMER:
            if (wParam == kTabDeliveryTimerId) {
                LONG phase = g_tabDeliveryPhase;
                if (phase == 1) {
                    Log("wnd TAB undo: injecting toggle");
                    g_tabSynthStartTick = GetTickCount();
                    InterlockedExchange(&g_tabDeliveryPhase, 2);
                    InjectTabEvent(false);
                    SetTimer(window, kTabDeliveryTimerId, kTabSynthTapMs, nullptr);
                } else {
                    KillTimer(window, kTabDeliveryTimerId);
                    if (phase == 2) {
                        InjectTabEvent(true);
                    }
                    InterlockedExchange(&g_tabDeliveryPhase, 0);
                }
                return 0;
            }
            break;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wParam == VK_TAB) {
                Log("wnd TAB keydown message=%u lParam=0x%08lX",
                    message, static_cast<unsigned long>(lParam));
                // Our own re-injected tap: hands off.
                if (TabSynthActive()) {
                    Log("wnd TAB keydown synth passthrough");
                    break;
                }
                // The TAB press of Alt+Tab or Win+Tab; the system switches
                // focus anyway, but SA-MP would toggle its scoreboard open
                // before that happens.
                if ((message == WM_SYSKEYDOWN && (HIWORD(lParam) & KF_ALTDOWN)) ||
                    ((GetKeyState(VK_MENU) | GetKeyState(VK_LWIN) |
                      GetKeyState(VK_RWIN)) & 0x8000)) {
                    Log("wnd alt-tab TAB keydown suppressed message=%u", message);
                    return 0;
                }
                if (TabRefocusGraceActive()) {
                    Log("wnd TAB keydown suppressed by refocus grace");
                    return 0;
                }
            }
            // A keydown of a key still held from before the refocus (e.g.
            // TAB released after Alt when tabbing back in) must not reach
            // the game as a fresh press. Queued autorepeats can be processed
            // after the physical release, so the mute lifts only on this
            // window's own WM_KEYUP or on a keydown without the repeat bit
            // (KF_REPEAT clear = genuinely new press).
            if (g_msgMutedKeys[wParam & 0xFF]) {
                if (HIWORD(lParam) & KF_REPEAT) {
                    Log("wnd keydown suppressed for held key vk=0x%02X message=%u",
                        static_cast<unsigned>(wParam), message);
                    return 0;
                }
                InterlockedExchange(&g_msgMutedKeys[wParam & 0xFF], 0);
            }
            // A real TAB press goes straight through; remember when, so a
            // focus loss right after it can schedule the undo tap.
            if (wParam == VK_TAB) {
                g_lastRealTabDownTick = GetTickCount();
            }
            break;

        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wParam == VK_TAB) {
                Log("wnd TAB keyup message=%u", message);
                if (TabSynthActive()) {
                    Log("wnd TAB keyup synth passthrough");
                    break;
                }
                if (TabRefocusGraceActive()) {
                    Log("wnd TAB keyup suppressed by refocus grace");
                    return 0;
                }
            }
            // The release of a muted key: the game never saw the press, so
            // it must not see the release either.
            if (g_msgMutedKeys[wParam & 0xFF]) {
                InterlockedExchange(&g_msgMutedKeys[wParam & 0xFF], 0);
                Log("wnd keyup swallowed for held key vk=0x%02X message=%u",
                    static_cast<unsigned>(wParam), message);
                return 0;
            }
            break;

        case WM_SETCURSOR:
            if (g_borderlessApplied &&
                LOWORD(lParam) == HTCLIENT && GetForegroundWindow() == window) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;

        case WM_STYLECHANGING:
            // Keep the game/RenderWare from restoring window borders.
            if (g_borderlessApplied && lParam) {
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
    }

    if (g_antiAfk) {
        switch (message) {
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    Log("anti-afk rewrote WM_ACTIVATE inactive -> active");
                    wParam = MAKEWPARAM(WA_ACTIVE, 0);
                }
                break;

            case WM_ACTIVATEAPP:
            case WM_NCACTIVATE:
                Log("anti-afk forced active message=%u", message);
                wParam = TRUE;
                break;

            case WM_KILLFOCUS:
                Log("anti-afk suppressed WM_KILLFOCUS");
                return 0;
        }
    }

    return ForwardToGame(window, message, wParam, lParam);
}

void InstallWindowHook(HWND window) {
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

using SetThreadDpiContextFn = HANDLE (WINAPI*)(HANDLE);

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

private:
    SetThreadDpiContextFn set_ = nullptr;
    HANDLE previous_ = nullptr;
};

void ApplyBorderlessStyle(HWND window) {
    if (!window || !IsWindow(window)) {
        Log("borderless skipped: invalid window=0x%p", window);
        return;
    }

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
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongW(window, GWL_STYLE, style);

    LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);
    LONG oldExStyle = exStyle;
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                 WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongW(window, GWL_EXSTYLE, exStyle);

    int width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    int height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;

    Log("borderless applying: window=0x%p monitor=(%ld,%ld)-(%ld,%ld) "
        "size=%dx%d style=0x%08lX->0x%08lX exstyle=0x%08lX->0x%08lX first=%d",
        window,
        monitor.rcMonitor.left, monitor.rcMonitor.top,
        monitor.rcMonitor.right, monitor.rcMonitor.bottom,
        width, height,
        oldStyle, style, oldExStyle, exStyle,
        g_borderlessApplied ? 0 : 1);

    SetWindowPos(window, HWND_TOP,
                 monitor.rcMonitor.left, monitor.rcMonitor.top,
                 width, height,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    if (!g_borderlessApplied) {
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

    g_borderlessApplied = true;
    Log("borderless applied");
}

void ApplyWindowedPresentParams(D3DPRESENT_PARAMETERS* params) {
    params->Windowed = TRUE;
    params->FullScreen_RefreshRateInHz = 0;
    params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    params->SwapEffect = D3DSWAPEFFECT_DISCARD;
    if (params->BackBufferFormat == D3DFMT_R5G6B5 ||
        params->BackBufferFormat == D3DFMT_X1R5G5B5 ||
        params->BackBufferFormat == D3DFMT_A1R5G5B5) {
        params->BackBufferFormat = D3DFMT_X8R8G8B8;
    }
}

// Fills `converted` with a fixed-up copy of the caller's parameters. The
// caller's struct is never written: for GTA SA it is a persistent global
// (0xC9C040) that other mods read and write, so in-place edits leak into
// their logic and can re-trigger their reset handling.
ConvertMode ConvertPresentParams(const D3DPRESENT_PARAMETERS* source,
                                 D3DPRESENT_PARAMETERS* converted) {
    __try {
        if (!source) {
            Log("convert skipped: params=null");
            return ConvertNone;
        }

        if (source->Windowed) {
            if (source->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE &&
                source->FullScreen_RefreshRateInHz == 0) {
                return ConvertNone;
            }

            *converted = *source;
            converted->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            // Windowed devices must not carry a fullscreen refresh rate.
            // Mods such as GameTweaker keep writing one into the game's
            // global present params, which real d3d9 rejects when windowed.
            converted->FullScreen_RefreshRateInHz = 0;
            return ConvertVsyncOnly;
        }

        *converted = *source;
        ApplyWindowedPresentParams(converted);
        return ConvertFullscreen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("convert failed with exception");
        return ConvertNone;
    }
}

void FormatCallerAddress(void* address, char* buffer, size_t size) {
    HMODULE module = nullptr;
    if (address &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module) {
        wchar_t path[MAX_PATH] = {};
        const wchar_t* name = path;
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t* slash = wcsrchr(path, L'\\');
            if (slash) {
                name = slash + 1;
            }
        }
        std::snprintf(buffer, size, "0x%p (%ls+0x%X)", address, name,
                      static_cast<unsigned>(reinterpret_cast<uintptr_t>(address) -
                                            reinterpret_cast<uintptr_t>(module)));
    } else {
        std::snprintf(buffer, size, "0x%p (unknown module)", address);
    }
}

HWND GetDeviceWindow(IDirect3DDevice9* device,
                     D3DPRESENT_PARAMETERS* params,
                     HWND focusWindow) {
    if (params && params->hDeviceWindow) {
        return params->hDeviceWindow;
    }

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    if (device &&
        SUCCEEDED(device->GetCreationParameters(&creation)) &&
        creation.hFocusWindow) {
        return creation.hFocusWindow;
    }

    return focusWindow;
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params);

void HookDeviceReset(IDirect3DDevice9* device) {
    if (g_originalReset || !device) {
        Log("reset hook skipped: existing=%d device=0x%p",
            g_originalReset ? 1 : 0, device);
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    void* target = vtable[kVtableReset];

    MH_STATUS create = MH_CreateHook(target, reinterpret_cast<void*>(&HookedReset),
                                     reinterpret_cast<void**>(&g_originalReset));
    MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
    Log("reset hook: target=0x%p create=%d enable=%d original=0x%p",
        target, create, enable, reinterpret_cast<void*>(g_originalReset));
    if (create != MH_OK || enable != MH_OK) {
        g_originalReset = nullptr;
    }
}

void AfterCreateDevice(IDirect3DDevice9* device,
                       D3DPRESENT_PARAMETERS* params,
                       HWND focusWindow,
                       ConvertMode mode) {
    __try {
        HookDeviceReset(device);

        HWND window = GetDeviceWindow(device, params, focusWindow);
        Log("after CreateDevice: device=0x%p window=0x%p focus=0x%p mode=%s",
            device, window, focusWindow, ConvertModeName(mode));
        RequireNextReset("CreateDevice");
        InstallWindowHook(window);
        InstallGetMessageHook(window);
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(window);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("after CreateDevice failed with exception");
    }
}

void AfterReset(IDirect3DDevice9* device,
                D3DPRESENT_PARAMETERS* params,
                ConvertMode mode) {
    __try {
        Log("after Reset: device=0x%p window=0x%p mode=%s",
            device, GetDeviceWindow(device, params, nullptr), ConvertModeName(mode));
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(GetDeviceWindow(device, params, nullptr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("after Reset failed with exception");
    }
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    void* caller = _ReturnAddress();

    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective = mode == ConvertNone ? params : &converted;

    bool redundant = IsRedundantReset(device, effective);
    if (redundant && g_haveForwardedReset) {
        ++g_suppressedRedundantResetCount;
        if (g_suppressedRedundantResetCount <= 10 ||
            (g_suppressedRedundantResetCount % 100) == 0) {
            char callerText[160] = {};
            FormatCallerAddress(caller, callerText, sizeof(callerText));
            Log("Reset return: suppressed redundant D3D_OK count=%d caller=%s",
                g_suppressedRedundantResetCount, callerText);
        }
        return D3D_OK;
    }

    char callerText[160] = {};
    FormatCallerAddress(caller, callerText, sizeof(callerText));
    Log("Reset enter: device=0x%p params=0x%p caller=%s", device, params, callerText);
    LogPresentParams("Reset input", params);
    if (mode != ConvertNone) {
        LogPresentParams("Reset converted", &converted);
    }
    Log("Reset conversion mode=%s", ConvertModeName(mode));

    if (redundant) {
        Log("Reset redundant-looking request: forwarding first required Reset");
    }

    HRESULT result = g_originalReset(device, effective);
    g_suppressedRedundantResetCount = 0;
    Log("Reset original result=0x%08lX", result);

    if (FAILED(result) && mode != ConvertNone) {
        LogPresentParams("Reset fallback input", params);
        result = g_originalReset(device, params);
        Log("Reset fallback result=0x%08lX", result);
        mode = ConvertNone;
        effective = params;
    }

    if (SUCCEEDED(result)) {
        RememberAppliedParams(effective);
        g_haveForwardedReset = true;
        if (mode != ConvertNone) {
            AfterReset(device, effective, mode);
        }
    }

    Log("Reset return: final result=0x%08lX mode=%s", result, ConvertModeName(mode));
    return result;
}

HRESULT WINAPI HookedCreateDevice(IDirect3D9* self,
                                  UINT adapter,
                                  D3DDEVTYPE deviceType,
                                  HWND focusWindow,
                                  DWORD behaviorFlags,
                                  D3DPRESENT_PARAMETERS* params,
                                  IDirect3DDevice9** device) {
    Log("CreateDevice enter: self=0x%p adapter=%u type=%u focus=0x%p behavior=0x%08lX params=0x%p out=0x%p",
        self, adapter, deviceType, focusWindow, behaviorFlags, params, device);
    LogPresentParams("CreateDevice input", params);

    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective = mode == ConvertNone ? params : &converted;
    if (mode != ConvertNone) {
        LogPresentParams("CreateDevice converted", &converted);
    }
    Log("CreateDevice conversion mode=%s", ConvertModeName(mode));

    HRESULT result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                            behaviorFlags, effective, device);
    Log("CreateDevice original result=0x%08lX device=0x%p",
        result, device ? *device : nullptr);

    if (FAILED(result) && mode != ConvertNone) {
        LogPresentParams("CreateDevice fallback input", params);
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, params, device);
        Log("CreateDevice fallback result=0x%08lX device=0x%p",
            result, device ? *device : nullptr);
        mode = ConvertNone;
        effective = params;
    }

    if (SUCCEEDED(result) && device && *device) {
        RememberAppliedParams(effective);
        AfterCreateDevice(*device, effective, focusWindow, mode);
    }

    Log("CreateDevice return: result=0x%08lX device=0x%p",
        result, device ? *device : nullptr);
    return result;
}

void HookCreateDevice(IDirect3D9* d3d) {
    if (!d3d || InterlockedCompareExchange(&g_createDeviceHookState, 1, 0) != 0) {
        Log("CreateDevice hook skipped: d3d=0x%p state=%ld",
            d3d, g_createDeviceHookState);
        return;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(d3d);
        void* target = vtable[kVtableCreateDevice];

        MH_STATUS create = MH_CreateHook(target, reinterpret_cast<void*>(&HookedCreateDevice),
                                         reinterpret_cast<void**>(&g_originalCreateDevice));
        MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
        Log("CreateDevice hook: d3d=0x%p target=0x%p create=%d enable=%d original=0x%p",
            d3d, target, create, enable,
            reinterpret_cast<void*>(g_originalCreateDevice));
        if (create == MH_OK && enable == MH_OK) {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CreateDevice hook failed with exception");
    }

    InterlockedExchange(&g_createDeviceHookState, 0);
    g_originalCreateDevice = nullptr;
}

bool BytesMatch(const void* address, const unsigned char* expected, size_t size) {
    __try {
        return std::memcmp(address, expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteGameCode(void* address, const unsigned char* bytes, size_t size) {
    DWORD oldProtect = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    bool ok = true;
    __try {
        std::memcpy(address, bytes, size);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    DWORD ignored = 0;
    VirtualProtect(address, size, oldProtect, &ignored);
    if (ok) {
        FlushInstructionCache(GetCurrentProcess(), address, size);
    }
    return ok;
}

void ApplyNoFrameDelay() {
    if (reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) != 0x00400000) {
        Log("NoFrameDelay skipped: module base is not 0x00400000");
        return;
    }

    struct Patch {
        uintptr_t     addr;
        unsigned char original[2];
        unsigned char patched[2];
        size_t        size;
    };
    const Patch patches[] = {
        { 0x0053E923, { 0xE8, 0x58 }, { 0xEB, 0x43 }, 2 },
        { 0x0053E99F, { 0x14, 0x00 }, { 0x10, 0x00 }, 1 },
        { 0x0053E9A5, { 0x5E, 0x00 }, { 0x90, 0x00 }, 1 },
    };

    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
        const Patch& p = patches[i];
        const void* addr = reinterpret_cast<const void*>(p.addr);
        if (!BytesMatch(addr, p.original, p.size) && !BytesMatch(addr, p.patched, p.size)) {
            Log("NoFrameDelay skipped: signature mismatch at 0x%08lX", p.addr);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
        const Patch& p = patches[i];
        void* addr = reinterpret_cast<void*>(p.addr);
        if (BytesMatch(addr, p.patched, p.size)) {
            Log("NoFrameDelay already patched at 0x%08lX", p.addr);
            continue;
        }
        if (!WriteGameCode(addr, p.patched, p.size)) {
            Log("NoFrameDelay failed to patch 0x%08lX", p.addr);
            return;
        }
        Log("NoFrameDelay patched 0x%08lX", p.addr);
    }
}

using ChangeDisplaySettingsAFn = LONG (WINAPI*)(DEVMODEA* devMode, DWORD flags);
using ChangeDisplaySettingsWFn = LONG (WINAPI*)(DEVMODEW* devMode, DWORD flags);
using ChangeDisplaySettingsExAFn = LONG (WINAPI*)(LPCSTR device, DEVMODEA* devMode,
                                                  HWND window, DWORD flags,
                                                  LPVOID param);
using ChangeDisplaySettingsExWFn = LONG (WINAPI*)(LPCWSTR device, DEVMODEW* devMode,
                                                  HWND window, DWORD flags,
                                                  LPVOID param);

ChangeDisplaySettingsAFn g_originalChangeDisplaySettingsA = nullptr;
ChangeDisplaySettingsWFn g_originalChangeDisplaySettingsW = nullptr;
ChangeDisplaySettingsExAFn g_originalChangeDisplaySettingsExA = nullptr;
ChangeDisplaySettingsExWFn g_originalChangeDisplaySettingsExW = nullptr;

void LogDisplayModeRequest(const char* label, void* caller,
                           DWORD fields, DWORD width, DWORD height,
                           DWORD frequency, DWORD flags, bool normalized) {
    char callerText[160] = {};
    FormatCallerAddress(caller, callerText, sizeof(callerText));
    Log("%s: mode=%ux%u@%u fields=0x%08lX flags=0x%08lX caller=%s%s",
        label, width, height, frequency, fields, flags, callerText,
        normalized ? " -> normalizing to desktop mode" : "");
}

// Suppressing these calls outright freezes the AppCompat path inside d3d9
// (DWM8And16BitMitigation shim), so forward them with the desktop mode
// instead: the caller gets a genuine success and no real mode switch happens.
void BuildDesktopDevModeW(DEVMODEW* out) {
    *out = g_desktopMode;
    out->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFREQUENCY;
}

void BuildDesktopDevModeA(DEVMODEA* out) {
    std::memset(out, 0, sizeof(*out));
    out->dmSize = sizeof(*out);
    out->dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFREQUENCY;
    out->dmPelsWidth = g_desktopMode.dmPelsWidth;
    out->dmPelsHeight = g_desktopMode.dmPelsHeight;
    out->dmBitsPerPel = g_desktopMode.dmBitsPerPel;
    out->dmDisplayFrequency = g_desktopMode.dmDisplayFrequency;
}

bool ReadDevModeA(const DEVMODEA* mode, DWORD* fields, DWORD* width,
                  DWORD* height, DWORD* frequency) {
    __try {
        if (mode) {
            *fields = mode->dmFields;
            *width = mode->dmPelsWidth;
            *height = mode->dmPelsHeight;
            *frequency = mode->dmDisplayFrequency;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadDevModeW(const DEVMODEW* mode, DWORD* fields, DWORD* width,
                  DWORD* height, DWORD* frequency) {
    __try {
        if (mode) {
            *fields = mode->dmFields;
            *width = mode->dmPelsWidth;
            *height = mode->dmPelsHeight;
            *frequency = mode->dmDisplayFrequency;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// While borderless is active the desktop mode must stay untouched. GTA SA
// (and the AppCompat shims around d3d9) still believe the game runs exclusive
// fullscreen and switch the display mode, causing slow alt-tabs with monitor
// re-syncs. Requests are forwarded with the desktop mode substituted in.
LONG WINAPI HookedChangeDisplaySettingsA(DEVMODEA* devMode, DWORD flags) {
    DWORD fields = 0, width = 0, height = 0, frequency = 0;
    ReadDevModeA(devMode, &fields, &width, &height, &frequency);
    bool normalize = g_borderlessApplied && devMode && g_haveDesktopMode;
    LogDisplayModeRequest("ChangeDisplaySettingsA", _ReturnAddress(),
                          fields, width, height, frequency, flags, normalize);
    if (normalize) {
        DEVMODEA desktop;
        BuildDesktopDevModeA(&desktop);
        return g_originalChangeDisplaySettingsA(&desktop, flags);
    }
    return g_originalChangeDisplaySettingsA(devMode, flags);
}

LONG WINAPI HookedChangeDisplaySettingsW(DEVMODEW* devMode, DWORD flags) {
    DWORD fields = 0, width = 0, height = 0, frequency = 0;
    ReadDevModeW(devMode, &fields, &width, &height, &frequency);
    bool normalize = g_borderlessApplied && devMode && g_haveDesktopMode;
    LogDisplayModeRequest("ChangeDisplaySettingsW", _ReturnAddress(),
                          fields, width, height, frequency, flags, normalize);
    if (normalize) {
        DEVMODEW desktop;
        BuildDesktopDevModeW(&desktop);
        return g_originalChangeDisplaySettingsW(&desktop, flags);
    }
    return g_originalChangeDisplaySettingsW(devMode, flags);
}

LONG WINAPI HookedChangeDisplaySettingsExA(LPCSTR device, DEVMODEA* devMode,
                                           HWND window, DWORD flags,
                                           LPVOID param) {
    DWORD fields = 0, width = 0, height = 0, frequency = 0;
    ReadDevModeA(devMode, &fields, &width, &height, &frequency);
    bool normalize = g_borderlessApplied && devMode && g_haveDesktopMode;
    LogDisplayModeRequest("ChangeDisplaySettingsExA", _ReturnAddress(),
                          fields, width, height, frequency, flags, normalize);
    if (normalize) {
        DEVMODEA desktop;
        BuildDesktopDevModeA(&desktop);
        return g_originalChangeDisplaySettingsExA(device, &desktop, window, flags,
                                                  param);
    }
    return g_originalChangeDisplaySettingsExA(device, devMode, window, flags, param);
}

LONG WINAPI HookedChangeDisplaySettingsExW(LPCWSTR device, DEVMODEW* devMode,
                                           HWND window, DWORD flags,
                                           LPVOID param) {
    DWORD fields = 0, width = 0, height = 0, frequency = 0;
    ReadDevModeW(devMode, &fields, &width, &height, &frequency);
    bool normalize = g_borderlessApplied && devMode && g_haveDesktopMode;
    LogDisplayModeRequest("ChangeDisplaySettingsExW", _ReturnAddress(),
                          fields, width, height, frequency, flags, normalize);
    if (normalize) {
        DEVMODEW desktop;
        BuildDesktopDevModeW(&desktop);
        return g_originalChangeDisplaySettingsExW(device, &desktop, window, flags,
                                                  param);
    }
    return g_originalChangeDisplaySettingsExW(device, devMode, window, flags, param);
}

// Walks the module's export table directly. GetProcAddress can be hooked by
// other mods and hand out redirected pointers; hooking those misses callers
// that resolved the real export through their import tables.
void* ResolveExportFromTable(HMODULE module, const char* name) {
    __try {
        const BYTE* base = reinterpret_cast<const BYTE*>(module);
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return nullptr;
        }
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return nullptr;
        }
        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size) {
            return nullptr;
        }
        const IMAGE_EXPORT_DIRECTORY* exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const DWORD* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const WORD* ordinals =
            reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const DWORD* functions =
            reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) {
                continue;
            }
            DWORD rva = functions[ordinals[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
                return nullptr;  // forwarded export
            }
            return const_cast<BYTE*>(base) + rva;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

void HookOneExport(HMODULE module, const char* name, void* hook, void** original) {
    void* fromGetProc = reinterpret_cast<void*>(GetProcAddress(module, name));
    void* fromTable = ResolveExportFromTable(module, name);
    void* target = fromTable ? fromTable : fromGetProc;
    if (!target) {
        Log("%s hook skipped: export not found", name);
        return;
    }
    char targetText[160] = {};
    FormatCallerAddress(target, targetText, sizeof(targetText));
    if (fromGetProc && fromGetProc != target) {
        char getProcText[160] = {};
        FormatCallerAddress(fromGetProc, getProcText, sizeof(getProcText));
        Log("%s export mismatch: table=%s GetProcAddress=%s (hooking table address)",
            name, targetText, getProcText);
    }
    MH_STATUS create = MH_CreateHook(target, hook, original);
    MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
    Log("%s hook: target=%s create=%d enable=%d", name, targetText, create, enable);
    if (create != MH_OK || enable != MH_OK) {
        *original = nullptr;
    }
}

void CaptureDesktopMode() {
    g_desktopMode.dmSize = sizeof(g_desktopMode);
    g_haveDesktopMode =
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &g_desktopMode) != FALSE;
    Log("desktop mode captured: %ux%u@%u bpp=%u have=%d",
        g_desktopMode.dmPelsWidth, g_desktopMode.dmPelsHeight,
        g_desktopMode.dmDisplayFrequency, g_desktopMode.dmBitsPerPel,
        g_haveDesktopMode ? 1 : 0);
}

// Someone (game, driver, another process) may still switch the real display
// mode behind the windowed device; put the desktop mode back immediately.
void RestoreDesktopMode(const char* reason) {
    if (!g_haveDesktopMode || !g_borderlessApplied) {
        return;
    }

    DEVMODEW current = {};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) {
        Log("display restore skipped: EnumDisplaySettings failed (%s)", reason);
        return;
    }

    Log("display mode check (%s): current=%ux%u@%u bpp=%u desktop=%ux%u@%u bpp=%u",
        reason,
        current.dmPelsWidth, current.dmPelsHeight,
        current.dmDisplayFrequency, current.dmBitsPerPel,
        g_desktopMode.dmPelsWidth, g_desktopMode.dmPelsHeight,
        g_desktopMode.dmDisplayFrequency, g_desktopMode.dmBitsPerPel);

    if (current.dmPelsWidth == g_desktopMode.dmPelsWidth &&
        current.dmPelsHeight == g_desktopMode.dmPelsHeight &&
        current.dmDisplayFrequency == g_desktopMode.dmDisplayFrequency &&
        current.dmBitsPerPel == g_desktopMode.dmBitsPerPel) {
        return;
    }

    if (!g_originalChangeDisplaySettingsW) {
        Log("display restore skipped: ChangeDisplaySettingsW trampoline missing");
        return;
    }

    DEVMODEW restore = g_desktopMode;
    LONG result = g_originalChangeDisplaySettingsW(&restore, 0);
    Log("display restore: result=%ld", result);
}

void HookChangeDisplaySettings() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    HookOneExport(user32, "ChangeDisplaySettingsA",
                  reinterpret_cast<void*>(&HookedChangeDisplaySettingsA),
                  reinterpret_cast<void**>(&g_originalChangeDisplaySettingsA));
    HookOneExport(user32, "ChangeDisplaySettingsW",
                  reinterpret_cast<void*>(&HookedChangeDisplaySettingsW),
                  reinterpret_cast<void**>(&g_originalChangeDisplaySettingsW));
    HookOneExport(user32, "ChangeDisplaySettingsExA",
                  reinterpret_cast<void*>(&HookedChangeDisplaySettingsExA),
                  reinterpret_cast<void**>(&g_originalChangeDisplaySettingsExA));
    HookOneExport(user32, "ChangeDisplaySettingsExW",
                  reinterpret_cast<void*>(&HookedChangeDisplaySettingsExW),
                  reinterpret_cast<void**>(&g_originalChangeDisplaySettingsExW));
}

BOOL WINAPI HookedSetCursorPos(int x, int y) {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &pid);
    }
    if (pid != GetCurrentProcessId()) {
        Log("SetCursorPos suppressed: x=%d y=%d foreground=0x%p pid=%lu",
            x, y, foreground, pid);
        return TRUE;
    }
    return g_originalSetCursorPos(x, y);
}

void HookSetCursorPos() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    void* target = reinterpret_cast<void*>(GetProcAddress(user32, "SetCursorPos"));
    if (!target) {
        Log("SetCursorPos hook skipped: export not found");
        return;
    }
    MH_STATUS create = MH_CreateHook(target, reinterpret_cast<void*>(&HookedSetCursorPos),
                                     reinterpret_cast<void**>(&g_originalSetCursorPos));
    MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
    Log("SetCursorPos hook: target=0x%p create=%d enable=%d original=0x%p",
        target, create, enable, reinterpret_cast<void*>(g_originalSetCursorPos));
    if (create != MH_OK || enable != MH_OK) {
        g_originalSetCursorPos = nullptr;
    }
}

using GetKeyStateFn = SHORT (WINAPI*)(int virtualKey);
using GetAsyncKeyStateFn = SHORT (WINAPI*)(int virtualKey);
using GetKeyboardStateFn = BOOL (WINAPI*)(PBYTE keyState);

GetKeyStateFn g_originalGetKeyState = nullptr;
GetAsyncKeyStateFn g_originalGetAsyncKeyState = nullptr;
GetKeyboardStateFn g_originalGetKeyboardState = nullptr;

volatile DWORD g_lastRefocusTick = 0;

// For a short window after the game regains focus, TAB reads as released on
// every input path. Buffered/queued input (DirectInput event queues, lagging
// key-state tables) can deliver the TAB of an Alt+Tab long after the key was
// physically released, past every state-based mute.
constexpr DWORD kTabRefocusGraceMs = 200;

bool TabRefocusGraceActive() {
    DWORD last = g_lastRefocusTick;
    return last != 0 && (GetTickCount() - last) < kTabRefocusGraceMs;
}

void LogTabDownRead(const char* api, void* caller, bool suppressed) {
    static volatile DWORD lastLogTick = 0;
    DWORD now = GetTickCount();
    DWORD last = lastLogTick;
    if (last != 0 && now - last < 250) {
        return;
    }
    lastLogTick = now;
    char callerText[160] = {};
    FormatCallerAddress(caller, callerText, sizeof(callerText));
    Log("TAB down read via %s caller=%s%s", api, callerText,
        suppressed ? " (suppressed)" : "");
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

// Windows delivers keyboard *messages* only to the focused window, but the
// game and SA-MP also poll the global key state every frame. With AntiAFK
// the game keeps simulating in the background, so typing in another window
// makes the character jump and fire. Polled key state is muted while the
// foreground window belongs to another process.
bool ForegroundBelongsToGame() {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &pid);
    }
    LONG game = pid == GetCurrentProcessId() ? 1 : 0;

    static LONG lastLogged = 1;
    if (InterlockedExchange(&lastLogged, game) != game) {
        Log("polled input %s: foreground=0x%p pid=%lu",
            game ? "restored" : "muted", foreground, pid);
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
        bool suppress = !TabSynthActive() &&
                        (altOrWin || TabRefocusGraceActive());
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
        bool altOrWin = ((g_originalGetAsyncKeyState(VK_MENU) |
                          g_originalGetAsyncKeyState(VK_LWIN) |
                          g_originalGetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
        bool suppress = !TabSynthActive() &&
                        (altOrWin || TabRefocusGraceActive());
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
                    bool suppress = !TabSynthActive() &&
                                    (altOrWin || TabRefocusGraceActive());
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

void HookKeyStateApis() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    HookOneExport(user32, "GetKeyState",
                  reinterpret_cast<void*>(&HookedGetKeyState),
                  reinterpret_cast<void**>(&g_originalGetKeyState));
    HookOneExport(user32, "GetAsyncKeyState",
                  reinterpret_cast<void*>(&HookedGetAsyncKeyState),
                  reinterpret_cast<void**>(&g_originalGetAsyncKeyState));
    HookOneExport(user32, "GetKeyboardState",
                  reinterpret_cast<void*>(&HookedGetKeyboardState),
                  reinterpret_cast<void**>(&g_originalGetKeyboardState));
}

// --- Message-queue TAB filter -----------------------------------------------
// SA-MP reads keyboard messages at pump level, before window-procedure
// dispatch, so WndProc-side suppression never reaches it. The queue itself
// is filtered instead: TAB messages that policy says to hide are rewritten
// to WM_NULL right where they are retrieved. Loaded-before-SA-MP hook
// ordering puts this filter between the real queue and SA-MP's reader for
// both an IAT hook and a later inline hook; a WH_GETMESSAGE hook installed
// after SA-MP's covers that route as well.

using PeekMessageTFn = BOOL (WINAPI*)(LPMSG msg, HWND window, UINT filterMin,
                                      UINT filterMax, UINT removeFlags);
using GetMessageTFn = BOOL (WINAPI*)(LPMSG msg, HWND window, UINT filterMin,
                                     UINT filterMax);

PeekMessageTFn g_originalPeekMessageA = nullptr;
PeekMessageTFn g_originalPeekMessageW = nullptr;
GetMessageTFn g_originalGetMessageA = nullptr;
GetMessageTFn g_originalGetMessageW = nullptr;
HHOOK g_getMessageHook = nullptr;

void FilterQueuedTabMessage(MSG* msg) {
    __try {
        if (!msg || msg->wParam != VK_TAB) {
            return;
        }
        bool down = msg->message == WM_KEYDOWN || msg->message == WM_SYSKEYDOWN;
        bool up = msg->message == WM_KEYUP || msg->message == WM_SYSKEYUP;
        if (!down && !up) {
            return;
        }
        if (TabSynthActive()) {
            return;
        }
        bool altOrWin = g_originalGetAsyncKeyState &&
                        ((g_originalGetAsyncKeyState(VK_MENU) |
                          g_originalGetAsyncKeyState(VK_LWIN) |
                          g_originalGetAsyncKeyState(VK_RWIN)) & 0x8000);
        if (altOrWin || TabRefocusGraceActive()) {
            Log("pump TAB %s neutralized (%s)", down ? "keydown" : "keyup",
                altOrWin ? "alt/win" : "grace");
            msg->message = WM_NULL;
            return;
        }
        if (down && !(HIWORD(msg->lParam) & KF_REPEAT)) {
            // This press WILL be seen by SA-MP; remember it so a focus loss
            // right after can schedule the undo tap.
            g_lastRealTabDownTick = GetTickCount();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

BOOL WINAPI HookedPeekMessageA(LPMSG msg, HWND window, UINT filterMin,
                               UINT filterMax, UINT removeFlags) {
    BOOL result = g_originalPeekMessageA(msg, window, filterMin, filterMax,
                                         removeFlags);
    if (result) {
        FilterQueuedTabMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedPeekMessageW(LPMSG msg, HWND window, UINT filterMin,
                               UINT filterMax, UINT removeFlags) {
    BOOL result = g_originalPeekMessageW(msg, window, filterMin, filterMax,
                                         removeFlags);
    if (result) {
        FilterQueuedTabMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedGetMessageA(LPMSG msg, HWND window, UINT filterMin,
                              UINT filterMax) {
    BOOL result = g_originalGetMessageA(msg, window, filterMin, filterMax);
    if (result != 0 && result != -1) {
        FilterQueuedTabMessage(msg);
    }
    return result;
}

BOOL WINAPI HookedGetMessageW(LPMSG msg, HWND window, UINT filterMin,
                              UINT filterMax) {
    BOOL result = g_originalGetMessageW(msg, window, filterMin, filterMax);
    if (result != 0 && result != -1) {
        FilterQueuedTabMessage(msg);
    }
    return result;
}

LRESULT CALLBACK GetMsgHookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        FilterQueuedTabMessage(reinterpret_cast<MSG*>(lParam));
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void HookMessagePump() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    HookOneExport(user32, "PeekMessageA",
                  reinterpret_cast<void*>(&HookedPeekMessageA),
                  reinterpret_cast<void**>(&g_originalPeekMessageA));
    HookOneExport(user32, "PeekMessageW",
                  reinterpret_cast<void*>(&HookedPeekMessageW),
                  reinterpret_cast<void**>(&g_originalPeekMessageW));
    HookOneExport(user32, "GetMessageA",
                  reinterpret_cast<void*>(&HookedGetMessageA),
                  reinterpret_cast<void**>(&g_originalGetMessageA));
    HookOneExport(user32, "GetMessageW",
                  reinterpret_cast<void*>(&HookedGetMessageW),
                  reinterpret_cast<void**>(&g_originalGetMessageW));
}

// Installed once the game window is known (after SA-MP set its hooks, so
// this one runs first in the WH_GETMESSAGE chain and neutralizes TAB before
// SA-MP's hook sees it).
void InstallGetMessageHook(HWND window) {
    if (g_getMessageHook || !window || !IsWindow(window)) {
        return;
    }
    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    g_getMessageHook = SetWindowsHookExW(WH_GETMESSAGE, &GetMsgHookProc,
                                         g_module, threadId);
    Log("WH_GETMESSAGE hook: hook=0x%p thread=%lu error=%lu",
        g_getMessageHook, threadId,
        g_getMessageHook ? 0 : GetLastError());
}

// --- DirectInput keyboard -------------------------------------------------
// SA-MP reads the keyboard through DirectInput as well; that path bypasses
// every user32 hook above. The buffered event queue can also hand out the
// TAB press of an Alt+Tab after the focus already returned to the game.

using DirectInput8CreateFn = HRESULT (WINAPI*)(HINSTANCE instance, DWORD version,
                                               REFIID iid, LPVOID* out,
                                               LPUNKNOWN outer);
using DICreateDeviceFn = HRESULT (WINAPI*)(void* self, REFGUID rguid,
                                           void** device, LPUNKNOWN outer);
using DIGetDeviceStateFn = HRESULT (WINAPI*)(void* self, DWORD size, LPVOID data);
using DIGetDeviceDataFn = HRESULT (WINAPI*)(void* self, DWORD objectSize,
                                            DIDEVICEOBJECTDATA* entries,
                                            DWORD* count, DWORD flags);

DirectInput8CreateFn g_originalDirectInput8Create = nullptr;
DICreateDeviceFn g_originalDICreateDevice = nullptr;
DIGetDeviceStateFn g_originalDIGetDeviceState = nullptr;
DIGetDeviceDataFn g_originalDIGetDeviceData = nullptr;
void* g_diCreateDeviceTarget = nullptr;
void* g_diGetDeviceStateTarget = nullptr;
void* g_diGetDeviceDataTarget = nullptr;

// GUID_SysKeyboard, spelled out to avoid linking dxguid.lib.
const GUID kGuidSysKeyboard =
    {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

constexpr DWORD kDikTab = 0x0F;
constexpr DWORD kDikLAlt = 0x38;
constexpr DWORD kDikRAlt = 0xB8;
constexpr int kMaxDiKeyboards = 8;

void* g_diKeyboards[kMaxDiKeyboards] = {};
LONG g_diKeyboardCount = 0;

bool IsDiKeyboard(void* device) {
    LONG count = g_diKeyboardCount;
    if (count > kMaxDiKeyboards) {
        count = kMaxDiKeyboards;
    }
    for (LONG i = 0; i < count; ++i) {
        if (g_diKeyboards[i] == device) {
            return true;
        }
    }
    return false;
}

void RememberDiKeyboard(void* device) {
    if (!device || IsDiKeyboard(device)) {
        return;
    }
    LONG index = InterlockedIncrement(&g_diKeyboardCount) - 1;
    if (index < kMaxDiKeyboards) {
        g_diKeyboards[index] = device;
        Log("DI keyboard device remembered: device=0x%p index=%ld", device, index);
    }
}

HRESULT WINAPI HookedDIGetDeviceState(void* self, DWORD size, LPVOID data) {
    HRESULT result = g_originalDIGetDeviceState(self, size, data);
    if (FAILED(result) || !data || !IsDiKeyboard(self)) {
        return result;
    }
    __try {
        BYTE* keys = static_cast<BYTE*>(data);
        if (!ForegroundBelongsToGame()) {
            std::memset(data, 0, size);
        } else if (size > kDikTab && (keys[kDikTab] & 0x80)) {
            bool altDown = (size > kDikRAlt) &&
                           ((keys[kDikLAlt] | keys[kDikRAlt]) & 0x80);
            bool suppress = altDown || TabRefocusGraceActive();
            LogTabDownRead("DI GetDeviceState", _ReturnAddress(), suppress);
            if (suppress) {
                keys[kDikTab] = 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

HRESULT WINAPI HookedDIGetDeviceData(void* self, DWORD objectSize,
                                     DIDEVICEOBJECTDATA* entries,
                                     DWORD* count, DWORD flags) {
    HRESULT result = g_originalDIGetDeviceData(self, objectSize, entries,
                                               count, flags);
    if (FAILED(result) || !count || !IsDiKeyboard(self)) {
        return result;
    }
    __try {
        if (!ForegroundBelongsToGame()) {
            *count = 0;
            return result;
        }
        if (entries && objectSize >= sizeof(DWORD) * 2) {
            BYTE* base = reinterpret_cast<BYTE*>(entries);
            DWORD total = *count;
            DWORD kept = 0;
            for (DWORD i = 0; i < total; ++i) {
                BYTE* entry = base + i * objectSize;
                DWORD offset = *reinterpret_cast<DWORD*>(entry);
                if (offset == kDikTab && TabRefocusGraceActive()) {
                    LogTabDownRead("DI GetDeviceData", _ReturnAddress(), true);
                    continue;
                }
                if (kept != i) {
                    std::memmove(base + kept * objectSize, entry, objectSize);
                }
                ++kept;
            }
            *count = kept;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

void HookDiDeviceMethods(void* device) {
    __try {
        void** vtable = *reinterpret_cast<void***>(device);

        void* stateTarget = vtable[9];   // IDirectInputDevice8::GetDeviceState
        if (!g_diGetDeviceStateTarget) {
            MH_STATUS create = MH_CreateHook(
                stateTarget, reinterpret_cast<void*>(&HookedDIGetDeviceState),
                reinterpret_cast<void**>(&g_originalDIGetDeviceState));
            MH_STATUS enable = create == MH_OK ? MH_EnableHook(stateTarget) : create;
            Log("DI GetDeviceState hook: target=0x%p create=%d enable=%d",
                stateTarget, create, enable);
            if (create == MH_OK && enable == MH_OK) {
                g_diGetDeviceStateTarget = stateTarget;
            } else {
                g_originalDIGetDeviceState = nullptr;
            }
        } else if (g_diGetDeviceStateTarget != stateTarget) {
            Log("DI GetDeviceState second vtable ignored: target=0x%p", stateTarget);
        }

        void* dataTarget = vtable[10];   // IDirectInputDevice8::GetDeviceData
        if (!g_diGetDeviceDataTarget) {
            MH_STATUS create = MH_CreateHook(
                dataTarget, reinterpret_cast<void*>(&HookedDIGetDeviceData),
                reinterpret_cast<void**>(&g_originalDIGetDeviceData));
            MH_STATUS enable = create == MH_OK ? MH_EnableHook(dataTarget) : create;
            Log("DI GetDeviceData hook: target=0x%p create=%d enable=%d",
                dataTarget, create, enable);
            if (create == MH_OK && enable == MH_OK) {
                g_diGetDeviceDataTarget = dataTarget;
            } else {
                g_originalDIGetDeviceData = nullptr;
            }
        } else if (g_diGetDeviceDataTarget != dataTarget) {
            Log("DI GetDeviceData second vtable ignored: target=0x%p", dataTarget);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("DI device hook failed with exception");
    }
}

HRESULT WINAPI HookedDICreateDevice(void* self, REFGUID rguid,
                                    void** device, LPUNKNOWN outer) {
    HRESULT result = g_originalDICreateDevice(self, rguid, device, outer);
    __try {
        bool keyboard = std::memcmp(&rguid, &kGuidSysKeyboard, sizeof(GUID)) == 0;
        char callerText[160] = {};
        FormatCallerAddress(_ReturnAddress(), callerText, sizeof(callerText));
        Log("DI CreateDevice: result=0x%08lX keyboard=%d device=0x%p caller=%s",
            result, keyboard ? 1 : 0,
            device ? *device : nullptr, callerText);
        if (SUCCEEDED(result) && keyboard && device && *device) {
            RememberDiKeyboard(*device);
            HookDiDeviceMethods(*device);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("DI CreateDevice post-processing failed with exception");
    }
    return result;
}

HRESULT WINAPI HookedDirectInput8Create(HINSTANCE instance, DWORD version,
                                        REFIID iid, LPVOID* out,
                                        LPUNKNOWN outer) {
    HRESULT result = g_originalDirectInput8Create(instance, version, iid, out, outer);
    __try {
        Log("DirectInput8Create: result=0x%08lX interface=0x%p",
            result, out ? *out : nullptr);
        if (SUCCEEDED(result) && out && *out) {
            void** vtable = *reinterpret_cast<void***>(*out);
            void* target = vtable[3];   // IDirectInput8::CreateDevice
            if (!g_diCreateDeviceTarget) {
                MH_STATUS create = MH_CreateHook(
                    target, reinterpret_cast<void*>(&HookedDICreateDevice),
                    reinterpret_cast<void**>(&g_originalDICreateDevice));
                MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
                Log("DI CreateDevice hook: target=0x%p create=%d enable=%d",
                    target, create, enable);
                if (create == MH_OK && enable == MH_OK) {
                    g_diCreateDeviceTarget = target;
                } else {
                    g_originalDICreateDevice = nullptr;
                }
            } else if (g_diCreateDeviceTarget != target) {
                Log("DI CreateDevice second vtable ignored: target=0x%p", target);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("DirectInput8Create post-processing failed with exception");
    }
    return result;
}

void HookDirectInput() {
    HMODULE dinput8 = LoadLibraryW(L"dinput8.dll");
    if (!dinput8) {
        Log("dinput8 LoadLibrary failed: error=%lu", GetLastError());
        return;
    }
    void* target = reinterpret_cast<void*>(
        GetProcAddress(dinput8, "DirectInput8Create"));
    if (!target) {
        Log("DirectInput8Create export not found");
        return;
    }
    MH_STATUS create = MH_CreateHook(
        target, reinterpret_cast<void*>(&HookedDirectInput8Create),
        reinterpret_cast<void**>(&g_originalDirectInput8Create));
    MH_STATUS enable = create == MH_OK ? MH_EnableHook(target) : create;
    Log("DirectInput8Create hook: target=0x%p create=%d enable=%d",
        target, create, enable);
    if (create != MH_OK || enable != MH_OK) {
        g_originalDirectInput8Create = nullptr;
    }
}

IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
    Log("Direct3DCreate9 enter: sdk=%u", sdkVersion);
    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    Log("Direct3DCreate9 result=0x%p", d3d);
    HookCreateDevice(d3d);
    return d3d;
}

void TryHookCreateDeviceThroughTemporaryObject(Direct3DCreate9Fn createD3D9) {
    __try {
        IDirect3D9* d3d = createD3D9(D3D_SDK_VERSION);
        Log("temporary D3D9 object=0x%p", d3d);
        if (d3d) {
            HookCreateDevice(d3d);
            d3d->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("temporary D3D9 hook attempt failed with exception");
    }
}

DWORD WINAPI Initialize(LPVOID) {
    LoadConfig();
    LogOpen();
    Log("initialize begin: module=0x%p antiAfk=%d log=%d",
        g_module, g_antiAfk ? 1 : 0, g_logEnabled ? 1 : 0);

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        Log("MinHook initialize failed: status=%d", status);
        return 0;
    }
    Log("MinHook initialized: status=%d", status);

    ApplyNoFrameDelay();
    CaptureDesktopMode();
    HookChangeDisplaySettings();

    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) {
        Log("d3d9 LoadLibrary failed: error=%lu", GetLastError());
        return 0;
    }
    Log("d3d9 loaded: module=0x%p", d3d9);

    auto createD3D9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!createD3D9) {
        Log("Direct3DCreate9 export not found");
        return 0;
    }
    Log("Direct3DCreate9 export=0x%p", reinterpret_cast<void*>(createD3D9));

    MH_STATUS create = MH_CreateHook(reinterpret_cast<void*>(createD3D9),
                                     reinterpret_cast<void*>(&HookedDirect3DCreate9),
                                     reinterpret_cast<void**>(&g_originalDirect3DCreate9));
    MH_STATUS enable = create == MH_OK
        ? MH_EnableHook(reinterpret_cast<void*>(createD3D9))
        : create;
    Log("Direct3DCreate9 hook: create=%d enable=%d original=0x%p",
        create, enable, reinterpret_cast<void*>(g_originalDirect3DCreate9));
    TryHookCreateDeviceThroughTemporaryObject(createD3D9);
    if (create == MH_OK && enable == MH_OK) {
        HookSetCursorPos();
        HookKeyStateApis();
        HookMessagePump();
        return 0;
    }

    HookSetCursorPos();
    HookKeyStateApis();
    HookMessagePump();
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        Log("process detach");
        if (g_getMessageHook) {
            UnhookWindowsHookEx(g_getMessageHook);
            g_getMessageHook = nullptr;
        }
        LogClose();
        if (g_logLockInitialized) {
            DeleteCriticalSection(&g_logLock);
            g_logLockInitialized = false;
        }
        return TRUE;
    }

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    g_module = instance;
    DisableThreadLibraryCalls(instance);

    InitializeCriticalSection(&g_logLock);
    g_logLockInitialized = true;

    HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        LogOpen();
        Log("initialize thread create failed: error=%lu", GetLastError());
    }

    return TRUE;
}
