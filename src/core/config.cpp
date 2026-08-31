#include "core/config.h"

#include "core/log.h"
#include "core/module.h"
#include "version.h"

#include <cstdlib>
#include <cwchar>

namespace bm {
namespace {

const wchar_t kGeneralSection[] = L"general";
const wchar_t kDebugSection[] = L"debug";
const wchar_t kFpsCounterSection[] = L"fpsCounter";
const wchar_t kLegacySection[] = L"BorderlessMode";
const wchar_t kOlderLegacySection[] = L"SABorderless";

// UTF-16 LE with BOM: the profile APIs read it natively, and text editors
// stop misdetecting the short file as UTF-16 mojibake.
const wchar_t kDefaultIni[] =
    L"\xFEFF"
    L"# BorderlessMode v" BM_VERSION_WIDE L"\r\n"
    L"# Created by sonochiwa\r\n"
    L"# Source code: https://github.com/sonochiwa/sa-borderless-mode\r\n"
    L"# Default FPS toggle hotkey: F11\r\n"
    L"\r\n"
    L"[general]\r\n"
    L"log=0\r\n"
    L"\r\n"
    L"# Declares the game DPI aware. Required for borderless mode on a display\r\n"
    L"# with scaling above 100%; without it Windows feeds the game a virtual\r\n"
    L"# desktop and the game shuts itself down during startup.\r\n"
    L"dpiAware=1\r\n"
    L"\r\n"
    L"# Shows GTA's actual in-game FPS. External tools such as NVIDIA\r\n"
    L"# counters may show the window's refresh rate instead.\r\n"
    L"[fpsCounter]\r\n"
    L"show=0\r\n"
    L"hotkeyEnabled=1\r\n"
    L"hotkeyModifier=0\r\n"
    L"hotkeyKey=122\r\n";

Config g_config;
wchar_t g_iniPath[MAX_PATH] = {};
volatile LONG g_showFpsOverlay = 0;

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

UINT ParseVirtualKey(const wchar_t* value, UINT fallback) {
    if (!value || !*value) {
        return fallback;
    }

    if ((value[0] == L'F' || value[0] == L'f') && value[1]) {
        wchar_t* end = nullptr;
        unsigned long functionNumber = wcstoul(value + 1, &end, 10);
        if (end && *end == L'\0' &&
            functionNumber >= 1 && functionNumber <= 24) {
            return VK_F1 + static_cast<UINT>(functionNumber - 1);
        }
    }

    wchar_t* end = nullptr;
    unsigned long numeric = wcstoul(value, &end, 0);
    if (end && *end == L'\0' && numeric >= 1 && numeric <= 0xFF) {
        return static_cast<UINT>(numeric);
    }

    return fallback;
}

int ReadIntSetting(const wchar_t* path, const wchar_t* section,
                   const wchar_t* key, const wchar_t* legacyKey,
                   int fallback) {
    int value = GetPrivateProfileIntW(section, key, -1, path);
    if (value >= 0) {
        return value;
    }

    value = GetPrivateProfileIntW(kLegacySection, legacyKey, -1, path);
    if (value >= 0) {
        return value;
    }

    return GetPrivateProfileIntW(kOlderLegacySection, legacyKey, fallback, path);
}

void ReadFpsHotkeyKey(const wchar_t* path, wchar_t* value, DWORD valueSize) {
    wchar_t sectionValues[64] = {};
    bool hasFpsCounterSection =
        GetPrivateProfileSectionW(kFpsCounterSection, sectionValues,
                                  static_cast<DWORD>(
                                      sizeof(sectionValues) /
                                      sizeof(sectionValues[0])),
                                  path) != 0;
    if (hasFpsCounterSection) {
        if (GetPrivateProfileStringW(kFpsCounterSection, L"hotkeyKey", L"",
                                     value, valueSize, path) != 0) {
            return;
        }
        // v1.3 development builds used this key in the new section.
        if (GetPrivateProfileStringW(kFpsCounterSection, L"toggleKey", L"",
                                     value, valueSize, path) != 0) {
            return;
        }
        // The new section is authoritative: a missing key disables the hotkey
        // even if stale legacy sections are still present in the file.
        value[0] = L'\0';
        return;
    }

    if (GetPrivateProfileStringW(kLegacySection, L"FpsToggleKey", L"",
                                 value, valueSize, path) != 0) {
        return;
    }
    if (GetPrivateProfileStringW(kOlderLegacySection, L"FpsToggleKey", L"",
                                 value, valueSize, path) != 0) {
        return;
    }
    value[0] = L'\0';
}

bool HasIniSetting(const wchar_t* path, const wchar_t* section,
                   const wchar_t* key) {
    wchar_t value[2] = {};
    return GetPrivateProfileStringW(section, key, L"", value,
                                    static_cast<DWORD>(
                                        sizeof(value) / sizeof(value[0])),
                                    path) != 0;
}

void SaveFpsOverlayState(LONG enabled) {
    wchar_t iniPath[MAX_PATH] = {};
    if (!BuildIniPath(iniPath, MAX_PATH)) {
        return;
    }

    const wchar_t* section = kFpsCounterSection;
    const wchar_t* key = L"show";
    if (!HasIniSetting(iniPath, section, key)) {
        if (HasIniSetting(iniPath, kLegacySection, L"ShowFPS")) {
            section = kLegacySection;
            key = L"ShowFPS";
        } else if (HasIniSetting(iniPath, kOlderLegacySection, L"ShowFPS")) {
            section = kOlderLegacySection;
            key = L"ShowFPS";
        }
    }

    const wchar_t* value = enabled ? L"1" : L"0";
    if (!WritePrivateProfileStringW(section, key, value, iniPath)) {
        Log("FPS overlay state save failed: error=%lu", GetLastError());
        return;
    }
    Log("FPS overlay state saved: enabled=%ld", enabled);
}

}  // namespace

void LoadConfig() {
    wchar_t iniPath[MAX_PATH] = {};
    if (!BuildIniPath(iniPath, MAX_PATH)) {
        return;
    }

    CreateDefaultIniIfMissing(iniPath);

    InterlockedExchange(
        &g_showFpsOverlay,
        ReadIntSetting(iniPath, kFpsCounterSection, L"show", L"ShowFPS", 0) != 0);
    g_config.fpsHotkeyEnabled =
        ReadIntSetting(iniPath, kFpsCounterSection, L"hotkeyEnabled",
                       L"FpsHotkeyEnabled", 1) != 0;
    g_config.fpsHotkeyModifier = static_cast<UINT>(
        ReadIntSetting(iniPath, kFpsCounterSection, L"hotkeyModifier",
                       L"FpsHotkeyModifier", 0));
    wchar_t fpsHotkeyKey[32] = {};
    ReadFpsHotkeyKey(
        iniPath, fpsHotkeyKey,
        static_cast<DWORD>(sizeof(fpsHotkeyKey) / sizeof(fpsHotkeyKey[0])));
    g_config.fpsHotkeyKey = ParseVirtualKey(fpsHotkeyKey, 0);
    g_config.logEnabled =
        ReadIntSetting(iniPath, kGeneralSection, L"log", L"Log", 0) != 0;
    LogSetEnabled(g_config.logEnabled);

    auto debugFlag = [&](const wchar_t* key) {
        return GetPrivateProfileIntW(kDebugSection, key, 0, iniPath) != 0;
    };
    g_config.disableWindowHook = debugFlag(L"disableWindowHook");
    g_config.disableInputFilters = debugFlag(L"disableInputFilters");
    g_config.disableMessagePump = debugFlag(L"disableMessagePump");
    g_config.disableCursorGuard = debugFlag(L"disableCursorGuard");
    g_config.disableDisplayGuard = debugFlag(L"disableDisplayGuard");
    g_config.disableGamePatches = debugFlag(L"disableGamePatches");
    g_config.disableBorderlessStyle = debugFlag(L"disableBorderlessStyle");
    g_config.disableConversion = debugFlag(L"disableConversion");
    g_config.dpiAware =
        GetPrivateProfileIntW(kGeneralSection, L"dpiAware", 1, iniPath) != 0;
    wcscpy_s(g_iniPath, iniPath);
}

const Config& GetConfig() {
    return g_config;
}

void LogConfigSummary() {
    Log("config loaded: ini=%ls Log=%d ShowFPS=%d "
        "FpsHotkeyEnabled=%d FpsHotkeyModifier=0x%02X FpsHotkeyKey=0x%02X",
        g_iniPath,
        g_config.logEnabled ? 1 : 0,
        ShowFpsOverlay() ? 1 : 0, g_config.fpsHotkeyEnabled ? 1 : 0,
        g_config.fpsHotkeyModifier, g_config.fpsHotkeyKey);
    Log("debug switches: window=%d input=%d pump=%d cursor=%d display=%d "
        "patches=%d style=%d convert=%d dpiAware=%d",
        g_config.disableWindowHook ? 1 : 0,
        g_config.disableInputFilters ? 1 : 0,
        g_config.disableMessagePump ? 1 : 0,
        g_config.disableCursorGuard ? 1 : 0,
        g_config.disableDisplayGuard ? 1 : 0,
        g_config.disableGamePatches ? 1 : 0,
        g_config.disableBorderlessStyle ? 1 : 0,
        g_config.disableConversion ? 1 : 0,
        g_config.dpiAware ? 1 : 0);
}

bool ShowFpsOverlay() {
    return InterlockedCompareExchange(&g_showFpsOverlay, 0, 0) != 0;
}

bool ToggleFpsOverlay() {
    LONG enabled = ShowFpsOverlay() ? 0 : 1;
    InterlockedExchange(&g_showFpsOverlay, enabled);
    SaveFpsOverlayState(enabled);
    return enabled != 0;
}

}  // namespace bm
