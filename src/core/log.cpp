#include "core/log.h"

#include "core/module.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace bm {
namespace {

bool g_logEnabled = false;
HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
bool g_logLockInitialized = false;
// Set before anything is torn down. The hooks installed by this plugin stay
// live for the whole life of the process, so they can still be entered while
// the process is shutting down.
volatile LONG g_shuttingDown = 0;

}  // namespace

void LogInit() {
    InitializeCriticalSection(&g_logLock);
    g_logLockInitialized = true;
}

void LogShutdown() {
    InterlockedExchange(&g_shuttingDown, 1);
}

void LogSetEnabled(bool enabled) {
    g_logEnabled = enabled;
}

bool LogEnabled() {
    return g_logEnabled;
}

bool LogActive() {
    return g_logFile != INVALID_HANDLE_VALUE;
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

void Log(const char* format, ...) {
    if (g_shuttingDown || g_logFile == INVALID_HANDLE_VALUE ||
        !g_logLockInitialized) {
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

void FormatCallerAddress(void* address, char* buffer, size_t size) {
    if (g_logFile == INVALID_HANDLE_VALUE) {
        std::snprintf(buffer, size, "0x%p", address);
        return;
    }

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

}  // namespace bm
