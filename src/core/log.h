#pragma once

#include <windows.h>

#include <cstddef>

namespace bm {

// Called from DllMain: prepares the lock before any hook can run.
void LogInit();
// Called from DLL_PROCESS_DETACH. Stops new writes without destroying the
// lock or the handle, both of which live threads may still be entering.
void LogShutdown();

void LogSetEnabled(bool enabled);
bool LogEnabled();
// Opens BorderlessMode.log next to the ASI when logging is enabled.
void LogOpen();
// True while the log file is open; guards work that only exists for logging.
bool LogActive();

void Log(const char* format, ...);

// Resolving an address to a module takes the loader lock. This runs from
// hooks on user32/d3d9 exports, which arbitrary threads reach - including
// threads that are already inside the loader initializing a DLL. Taking the
// loader lock there can deadlock the process, so the lookup only happens
// while diagnostics are actually being written.
void FormatCallerAddress(void* address, char* buffer, size_t size);

// Size that comfortably holds any FormatCallerAddress output.
constexpr size_t kCallerTextSize = 160;

}  // namespace bm
