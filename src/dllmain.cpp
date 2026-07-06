#include <windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "MinHook.h"

namespace {

constexpr int kVtableCreateDevice = 16;
constexpr int kVtableReset = 16;

const wchar_t kIniSection[] = L"BorderlessMode";
const wchar_t kLegacyIniSection[] = L"SABorderless";

const char kDefaultIni[] =
    "[BorderlessMode]\r\n"
    "AntiAFK=0\r\n"
    "Log=1\r\n";

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
    WriteFile(file, kDefaultIni, sizeof(kDefaultIni) - 1, &written, nullptr);
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

const LONG kFrameStyleBits = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                             WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME;
const LONG kFrameExStyleBits = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                               WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

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
            return 0;

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

    if (!g_previousWndProc) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    if (g_windowIsUnicode) {
        return CallWindowProcW(g_previousWndProc, window, message, wParam, lParam);
    }

    return CallWindowProcA(g_previousWndProc, window, message, wParam, lParam);
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

ConvertMode ConvertPresentParams(D3DPRESENT_PARAMETERS* params,
                                 D3DPRESENT_PARAMETERS* backup) {
    __try {
        if (!params) {
            Log("convert skipped: params=null");
            return ConvertNone;
        }

        if (params->Windowed) {
            if (params->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE) {
                return ConvertNone;
            }

            *backup = *params;
            params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            return ConvertVsyncOnly;
        }

        *backup = *params;
        ApplyWindowedPresentParams(params);
        return ConvertFullscreen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("convert failed with exception");
        return ConvertNone;
    }
}

bool RestorePresentParams(D3DPRESENT_PARAMETERS* params,
                          const D3DPRESENT_PARAMETERS* backup) {
    __try {
        *params = *backup;
        Log("present params restored for fallback");
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("present params restore failed with exception");
        return false;
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
    D3DPRESENT_PARAMETERS original = {};
    ConvertMode mode = ConvertPresentParams(params, &original);

    bool redundant = IsRedundantReset(device, params);
    if (redundant && g_haveForwardedReset) {
        ++g_suppressedRedundantResetCount;
        if (g_suppressedRedundantResetCount <= 10 ||
            (g_suppressedRedundantResetCount % 100) == 0) {
            Log("Reset return: suppressed redundant D3D_OK count=%d",
                g_suppressedRedundantResetCount);
        }
        return D3D_OK;
    }

    Log("Reset enter: device=0x%p params=0x%p", device, params);
    LogPresentParams("Reset input", mode == ConvertNone ? params : &original);
    LogPresentParams("Reset converted", params);
    Log("Reset conversion mode=%s", ConvertModeName(mode));

    if (redundant) {
        Log("Reset redundant-looking request: forwarding first required Reset");
    }

    HRESULT result = g_originalReset(device, params);
    g_suppressedRedundantResetCount = 0;
    Log("Reset original result=0x%08lX", result);

    if (mode == ConvertNone) {
        if (SUCCEEDED(result)) {
            RememberAppliedParams(params);
            g_haveForwardedReset = true;
        }
        Log("Reset return: result=0x%08lX", result);
        return result;
    }

    if (SUCCEEDED(result)) {
        RememberAppliedParams(params);
        g_haveForwardedReset = true;
        AfterReset(device, params, mode);
        Log("Reset return: converted result=0x%08lX", result);
        return result;
    }

    if (RestorePresentParams(params, &original)) {
        LogPresentParams("Reset fallback input", params);
        result = g_originalReset(device, params);
        Log("Reset fallback result=0x%08lX", result);
        if (SUCCEEDED(result)) {
            RememberAppliedParams(params);
            g_haveForwardedReset = true;
        }
    }

    Log("Reset return: final result=0x%08lX", result);
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

    D3DPRESENT_PARAMETERS original = {};
    ConvertMode mode = ConvertPresentParams(params, &original);
    LogPresentParams("CreateDevice converted", params);
    Log("CreateDevice conversion mode=%s", ConvertModeName(mode));

    HRESULT result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                            behaviorFlags, params, device);
    Log("CreateDevice original result=0x%08lX device=0x%p",
        result, device ? *device : nullptr);

    if (mode != ConvertNone && FAILED(result) && RestorePresentParams(params, &original)) {
        LogPresentParams("CreateDevice fallback input", params);
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, params, device);
        Log("CreateDevice fallback result=0x%08lX device=0x%p",
            result, device ? *device : nullptr);
    }

    if (SUCCEEDED(result) && device && *device) {
        RememberAppliedParams(params);
        AfterCreateDevice(*device, params, focusWindow, mode);
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
    HookSetCursorPos();

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
    if (create == MH_OK && enable == MH_OK) {
        return 0;
    }

    TryHookCreateDeviceThroughTemporaryObject(createD3D9);
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        Log("process detach");
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
