#include "window/display_mode.h"

#include "core/hook.h"
#include "window/borderless.h"

#include <intrin.h>
#include <windows.h>

#include <cstring>

namespace bm {
namespace {

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

DEVMODEW g_desktopMode = {};
bool g_haveDesktopMode = false;

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

// A mode switch request is replaced by the desktop mode whenever the game is
// running borderless and a real desktop mode is known.
bool ShouldNormalize(const void* devMode) {
    return BorderlessApplied() && devMode != nullptr && g_haveDesktopMode;
}

LONG WINAPI HookedChangeDisplaySettingsA(DEVMODEA* devMode, DWORD flags) {
    DWORD fields = 0, width = 0, height = 0, frequency = 0;
    ReadDevModeA(devMode, &fields, &width, &height, &frequency);
    bool normalize = ShouldNormalize(devMode);
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
    bool normalize = ShouldNormalize(devMode);
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
    bool normalize = ShouldNormalize(devMode);
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
    bool normalize = ShouldNormalize(devMode);
    if (normalize) {
        DEVMODEW desktop;
        BuildDesktopDevModeW(&desktop);
        return g_originalChangeDisplaySettingsExW(device, &desktop, window, flags,
                                                  param);
    }
    return g_originalChangeDisplaySettingsExW(device, devMode, window, flags, param);
}

}  // namespace

void CaptureDesktopMode() {
    g_desktopMode.dmSize = sizeof(g_desktopMode);
    g_haveDesktopMode =
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &g_desktopMode) != FALSE;
}

void RestoreDesktopMode() {
    if (!g_haveDesktopMode || !BorderlessApplied()) {
        return;
    }

    DEVMODEW current = {};
    current.dmSize = sizeof(current);
    if (!EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &current)) {
        return;
    }

    if (current.dmPelsWidth == g_desktopMode.dmPelsWidth &&
        current.dmPelsHeight == g_desktopMode.dmPelsHeight &&
        current.dmDisplayFrequency == g_desktopMode.dmDisplayFrequency &&
        current.dmBitsPerPel == g_desktopMode.dmBitsPerPel) {
        return;
    }

    if (!g_originalChangeDisplaySettingsW) {
        return;
    }

    DEVMODEW restore = g_desktopMode;
    g_originalChangeDisplaySettingsW(&restore, 0);
}

void HookChangeDisplaySettings() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return;
    }
    InstallExportHook(user32, "ChangeDisplaySettingsA",
                      reinterpret_cast<void*>(&HookedChangeDisplaySettingsA),
                      reinterpret_cast<void**>(&g_originalChangeDisplaySettingsA));
    InstallExportHook(user32, "ChangeDisplaySettingsW",
                      reinterpret_cast<void*>(&HookedChangeDisplaySettingsW),
                      reinterpret_cast<void**>(&g_originalChangeDisplaySettingsW));
    InstallExportHook(user32, "ChangeDisplaySettingsExA",
                      reinterpret_cast<void*>(&HookedChangeDisplaySettingsExA),
                      reinterpret_cast<void**>(&g_originalChangeDisplaySettingsExA));
    InstallExportHook(user32, "ChangeDisplaySettingsExW",
                      reinterpret_cast<void*>(&HookedChangeDisplaySettingsExW),
                      reinterpret_cast<void**>(&g_originalChangeDisplaySettingsExW));
}

}  // namespace bm
