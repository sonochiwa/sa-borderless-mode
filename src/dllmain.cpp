#include <windows.h>
#include <d3d9.h>

#include <cstring>

#include "MinHook.h"

namespace {

constexpr int kVtableCreateDevice = 16;
constexpr int kVtableReset = 16;

const wchar_t kIniSection[] = L"BorderlessMode";
const wchar_t kLegacyIniSection[] = L"SABorderless";

const wchar_t kDefaultIni[] =
    L"\xFEFF[BorderlessMode]\r\n"
    L"; AntiAFK=1 keeps the game running while minimized or in the background.\r\n"
    L"; DirectInput still prevents keyboard input from leaking into the game.\r\n"
    L"AntiAFK=0\r\n";

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
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsRedundantReset(IDirect3DDevice9* device, const D3DPRESENT_PARAMETERS* params) {
    if (!device || !params || !g_haveAppliedParams) {
        return false;
    }
    __try {
        return SameDisplayMode(*params, g_appliedParams) &&
               device->TestCooperativeLevel() == D3D_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BuildIniPath(wchar_t* path, DWORD pathSize) {
    DWORD length = GetModuleFileNameW(g_module, path, pathSize);
    if (length == 0 || length >= pathSize) {
        return false;
    }

    wchar_t* extension = wcsrchr(path, L'.');
    if (!extension) {
        return false;
    }

    wcscpy_s(extension, pathSize - (extension - path), L".ini");
    return true;
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
    WriteFile(file, kDefaultIni, sizeof(kDefaultIni) - sizeof(wchar_t), &written, nullptr);
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
}

bool g_borderlessApplied = false;

const LONG kFrameStyleBits = WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                             WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME;
const LONG kFrameExStyleBits = WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                               WS_EX_CLIENTEDGE | WS_EX_STATICEDGE;

LRESULT CALLBACK GameWndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SETFOCUS:
            // Avoid the vanilla restore-from-tray ESC menu.
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
                    style->styleNew &= ~static_cast<DWORD>(kFrameStyleBits);
                    style->styleNew |= WS_POPUP;
                } else if (wParam == static_cast<WPARAM>(GWL_EXSTYLE)) {
                    style->styleNew &= ~static_cast<DWORD>(kFrameExStyleBits);
                }
            }
            break;
    }

    if (g_antiAfk) {
        switch (message) {
            case WM_ACTIVATE:
                if (LOWORD(wParam) == WA_INACTIVE) {
                    wParam = MAKEWPARAM(WA_ACTIVE, 0);
                }
                break;

            case WM_ACTIVATEAPP:
            case WM_NCACTIVATE:
                wParam = TRUE;
                break;

            case WM_KILLFOCUS:
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
        return;
    }

    g_windowIsUnicode = IsWindowUnicode(window);

    LONG_PTR hook = reinterpret_cast<LONG_PTR>(&GameWndProc);
    LONG_PTR previous = g_windowIsUnicode
        ? SetWindowLongPtrW(window, GWLP_WNDPROC, hook)
        : SetWindowLongPtrA(window, GWLP_WNDPROC, hook);

    if (previous) {
        g_previousWndProc = reinterpret_cast<WNDPROC>(previous);
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
        return;
    }

    ScopedDpiAwareness dpiAware;

    MONITORINFO monitor = {};
    monitor.cbSize = sizeof(monitor);

    HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(handle, &monitor)) {
        return;
    }

    LONG style = GetWindowLongW(window, GWL_STYLE);
    style &= ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
               WS_SYSMENU | WS_BORDER | WS_DLGFRAME);
    style |= WS_POPUP | WS_VISIBLE;
    SetWindowLongW(window, GWL_STYLE, style);

    LONG exStyle = GetWindowLongW(window, GWL_EXSTYLE);
    exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                 WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);
    SetWindowLongW(window, GWL_EXSTYLE, exStyle);

    int width = monitor.rcMonitor.right - monitor.rcMonitor.left;
    int height = monitor.rcMonitor.bottom - monitor.rcMonitor.top;

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
    }

    g_borderlessApplied = true;
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
        return ConvertNone;
    }
}

bool RestorePresentParams(D3DPRESENT_PARAMETERS* params,
                          const D3DPRESENT_PARAMETERS* backup) {
    __try {
        *params = *backup;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
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
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    void* target = vtable[kVtableReset];

    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedReset),
                      reinterpret_cast<void**>(&g_originalReset)) == MH_OK) {
        MH_EnableHook(target);
    }
}

void AfterCreateDevice(IDirect3DDevice9* device,
                       D3DPRESENT_PARAMETERS* params,
                       HWND focusWindow,
                       ConvertMode mode) {
    __try {
        HookDeviceReset(device);

        HWND window = GetDeviceWindow(device, params, focusWindow);
        InstallWindowHook(window);
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(window);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AfterReset(IDirect3DDevice9* device,
                D3DPRESENT_PARAMETERS* params,
                ConvertMode mode) {
    __try {
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(GetDeviceWindow(device, params, nullptr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
    D3DPRESENT_PARAMETERS original = {};
    ConvertMode mode = ConvertPresentParams(params, &original);

    if (IsRedundantReset(device, params)) {
        return D3D_OK;
    }

    HRESULT result = g_originalReset(device, params);

    if (mode == ConvertNone) {
        if (SUCCEEDED(result)) {
            RememberAppliedParams(params);
        }
        return result;
    }

    if (SUCCEEDED(result)) {
        RememberAppliedParams(params);
        AfterReset(device, params, mode);
        return result;
    }

    if (RestorePresentParams(params, &original)) {
        result = g_originalReset(device, params);
        if (SUCCEEDED(result)) {
            RememberAppliedParams(params);
        }
    }

    return result;
}

HRESULT WINAPI HookedCreateDevice(IDirect3D9* self,
                                  UINT adapter,
                                  D3DDEVTYPE deviceType,
                                  HWND focusWindow,
                                  DWORD behaviorFlags,
                                  D3DPRESENT_PARAMETERS* params,
                                  IDirect3DDevice9** device) {
    D3DPRESENT_PARAMETERS original = {};
    ConvertMode mode = ConvertPresentParams(params, &original);

    HRESULT result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                            behaviorFlags, params, device);

    if (mode != ConvertNone && FAILED(result) && RestorePresentParams(params, &original)) {
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, params, device);
    }

    if (SUCCEEDED(result) && device && *device) {
        RememberAppliedParams(params);
        AfterCreateDevice(*device, params, focusWindow, mode);
    }

    return result;
}

void HookCreateDevice(IDirect3D9* d3d) {
    if (!d3d || InterlockedCompareExchange(&g_createDeviceHookState, 1, 0) != 0) {
        return;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(d3d);
        void* target = vtable[kVtableCreateDevice];

        if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedCreateDevice),
                          reinterpret_cast<void**>(&g_originalCreateDevice)) == MH_OK &&
            MH_EnableHook(target) == MH_OK) {
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    InterlockedExchange(&g_createDeviceHookState, 0);
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
            return;
        }
    }

    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
        const Patch& p = patches[i];
        void* addr = reinterpret_cast<void*>(p.addr);
        if (BytesMatch(addr, p.patched, p.size)) {
            continue;
        }
        if (!WriteGameCode(addr, p.patched, p.size)) {
            return;
        }
    }
}

BOOL WINAPI HookedSetCursorPos(int x, int y) {
    HWND foreground = GetForegroundWindow();
    DWORD pid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &pid);
    }
    if (pid != GetCurrentProcessId()) {
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
        return;
    }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&HookedSetCursorPos),
                      reinterpret_cast<void**>(&g_originalSetCursorPos)) == MH_OK) {
        MH_EnableHook(target);
    }
}

IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    HookCreateDevice(d3d);
    return d3d;
}

void TryHookCreateDeviceThroughTemporaryObject(Direct3DCreate9Fn createD3D9) {
    __try {
        IDirect3D9* d3d = createD3D9(D3D_SDK_VERSION);
        if (d3d) {
            HookCreateDevice(d3d);
            d3d->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

DWORD WINAPI Initialize(LPVOID) {
    LoadConfig();

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return 0;
    }

    ApplyNoFrameDelay();
    HookSetCursorPos();

    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    if (!d3d9) {
        return 0;
    }

    auto createD3D9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!createD3D9) {
        return 0;
    }

    if (MH_CreateHook(reinterpret_cast<void*>(createD3D9),
                      reinterpret_cast<void*>(&HookedDirect3DCreate9),
                      reinterpret_cast<void**>(&g_originalDirect3DCreate9)) == MH_OK &&
        MH_EnableHook(reinterpret_cast<void*>(createD3D9)) == MH_OK) {
        return 0;
    }

    TryHookCreateDeviceThroughTemporaryObject(createD3D9);
    return 0;
}

}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    g_module = instance;
    DisableThreadLibraryCalls(instance);

    HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }

    return TRUE;
}
