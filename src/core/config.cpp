#include "core/config.h"

#include "core/module.h"
#include "resource.h"

namespace bm {
namespace {

const wchar_t kFpsCounterSection[] = L"fpsCounter";

Config g_config;
wchar_t g_iniPath[MAX_PATH] = {};
volatile LONG g_showFpsCounter = 0;

// Writes the RCDATA copy of Config\BorderlessMode.ini byte for byte.
void CreateDefaultIniIfMissing(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    const HMODULE self = SelfModule();
    const HRSRC resource =
        FindResourceW(self, MAKEINTRESOURCEW(IDR_DEFAULT_INI), RT_RCDATA);
    if (!resource) {
        return;
    }
    const HGLOBAL handle = LoadResource(self, resource);
    const DWORD size = SizeofResource(self, resource);
    const void* data = handle ? LockResource(handle) : nullptr;
    if (!data || size == 0) {
        return;
    }

    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(file, data, size, &written, nullptr);
    CloseHandle(file);
}

// Runs on a pool thread. Writes whatever the flag currently says rather than
// a captured value, so two toggles in quick succession converge on the real
// state whichever order their work items run in.
DWORD WINAPI SaveFpsCounterStateWorker(LPVOID) {
    const LONG shown = InterlockedCompareExchange(&g_showFpsCounter, 0, 0);
    WritePrivateProfileStringW(kFpsCounterSection, L"show",
                               shown ? L"1" : L"0", g_iniPath);
    return 0;
}

}  // namespace

void LoadConfig() {
    if (!BuildSiblingPath(L".ini", g_iniPath, MAX_PATH)) {
        g_iniPath[0] = L'\0';
        return;
    }

    CreateDefaultIniIfMissing(g_iniPath);

    InterlockedExchange(
        &g_showFpsCounter,
        GetPrivateProfileIntW(kFpsCounterSection, L"show", 0, g_iniPath) != 0);
    g_config.fpsHotkeyEnabled =
        GetPrivateProfileIntW(kFpsCounterSection, L"hotkeyEnabled", 1,
                              g_iniPath) != 0;
    g_config.fpsHotkeyModifier = static_cast<UINT>(
        GetPrivateProfileIntW(kFpsCounterSection, L"hotkeyModifier", VK_MENU,
                              g_iniPath));
    // A missing key keeps the default; a present 0 disables the hotkey.
    g_config.fpsHotkeyKey = static_cast<UINT>(
        GetPrivateProfileIntW(kFpsCounterSection, L"hotkeyKey", VK_F11,
                              g_iniPath));
}

const Config& GetConfig() {
    return g_config;
}

bool ShowFpsCounter() {
    return InterlockedCompareExchange(&g_showFpsCounter, 0, 0) != 0;
}

bool ToggleFpsCounter() {
    const LONG shown = ShowFpsCounter() ? 0 : 1;
    InterlockedExchange(&g_showFpsCounter, shown);

    // Toggled from the window procedure, on the game thread. Writing the INI
    // there stalls a frame on file I/O for a setting nothing is waiting for,
    // so it goes to a pool thread.
    if (g_iniPath[0] &&
        !QueueUserWorkItem(&SaveFpsCounterStateWorker, nullptr,
                           WT_EXECUTEDEFAULT)) {
        SaveFpsCounterStateWorker(nullptr);
    }
    return shown != 0;
}

}  // namespace bm
