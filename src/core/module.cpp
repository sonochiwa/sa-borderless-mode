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

void PinSelf() {
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN |
                           GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&PinSelf), &pinned);
}

}  // namespace bm
