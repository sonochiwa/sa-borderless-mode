#include "core/module.h"

#include <cwchar>

namespace bm {
namespace {

HMODULE g_module = nullptr;

}  // namespace

void SetSelfModule(HMODULE module) {
    g_module = module;
}

HMODULE SelfModule() {
    return g_module;
}

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

bool MakeProcessDpiAware() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return false;
    }

    // Win10 1703+. DPI_AWARENESS_CONTEXT_SYSTEM_AWARE is -3, which is what the
    // HIGHDPIAWARE compatibility layer applies; per-monitor awareness would
    // also make the game responsible for DPI changes it knows nothing about.
    using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(HANDLE);
    auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext && setContext(reinterpret_cast<HANDLE>(-3))) {
        return true;
    }

    // Vista+. Fails harmlessly when awareness is already set, by this call,
    // by a compatibility layer or by a manifest.
    using SetProcessDPIAwareFn = BOOL (WINAPI*)(void);
    auto setAware = reinterpret_cast<SetProcessDPIAwareFn>(
        GetProcAddress(user32, "SetProcessDPIAware"));
    return setAware && setAware() != FALSE;
}

bool IsProcessDpiAwareNow() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) {
        return false;
    }
    using IsProcessDPIAwareFn = BOOL (WINAPI*)(void);
    auto isAware = reinterpret_cast<IsProcessDPIAwareFn>(
        GetProcAddress(user32, "IsProcessDPIAware"));
    return isAware && isAware() != FALSE;
}

void PinSelf() {
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinSelf), &pinned);
}

}  // namespace bm
