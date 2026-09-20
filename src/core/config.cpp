#include "core/config.h"

#include "core/module.h"
#include "resource.h"

#include <windows.h>

namespace bm {
namespace {

const wchar_t kFpsCounterSection[] = L"fpsCounter";
const wchar_t kDefaultCommand[] = L"FPSCOUNTER";

constexpr size_t kCommandCapacity = 32;

wchar_t g_iniPath[MAX_PATH] = {};
char g_command[kCommandCapacity] = {};
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

// Keeps the letters and digits of the configured word, upper case, the form
// the message pump filter compares virtual-key codes against.
void ReadCommand(const wchar_t* path) {
    wchar_t value[kCommandCapacity] = {};
    // A missing key keeps the compiled default; a present empty one disables
    // the command rather than falling back to it.
    GetPrivateProfileStringW(kFpsCounterSection, L"command", kDefaultCommand,
                             value, kCommandCapacity, path);

    size_t length = 0;
    for (const wchar_t* c = value; *c && length < kCommandCapacity - 1; ++c) {
        if (*c >= L'a' && *c <= L'z') {
            g_command[length++] = static_cast<char>(*c - L'a' + 'A');
        } else if ((*c >= L'A' && *c <= L'Z') || (*c >= L'0' && *c <= L'9')) {
            g_command[length++] = static_cast<char>(*c);
        }
    }
    g_command[length] = '\0';
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
    ReadCommand(g_iniPath);
}

const char* FpsCounterCommand() {
    return g_command;
}

bool ShowFpsCounter() {
    return InterlockedCompareExchange(&g_showFpsCounter, 0, 0) != 0;
}

bool ToggleFpsCounter() {
    const LONG shown = ShowFpsCounter() ? 0 : 1;
    InterlockedExchange(&g_showFpsCounter, shown);

    // Toggled on the game thread. Writing the INI there stalls a frame on
    // file I/O for a setting nothing is waiting for, so it goes to a pool
    // thread.
    if (g_iniPath[0] &&
        !QueueUserWorkItem(&SaveFpsCounterStateWorker, nullptr,
                           WT_EXECUTEDEFAULT)) {
        SaveFpsCounterStateWorker(nullptr);
    }
    return shown != 0;
}

}  // namespace bm
