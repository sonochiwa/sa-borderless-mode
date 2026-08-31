#pragma once

#include <windows.h>

#include <cstddef>

namespace bm {

// Creates a MinHook detour and queues it for activation, logging the outcome
// under `name`. `*original` is cleared when the hook could not be created, so
// callers can treat a null trampoline as "feature unavailable".
//
// The hook is not live until ApplyQueuedHooks() runs.
bool InstallHook(void* target, void* detour, void** original, const char* name);

// Activates every queued hook in a single pass.
//
// This is why the hooks are queued at all: MH_EnableHook suspends every thread
// in the process, walks their instruction pointers and resumes them, once per
// hook. Installing the plugin's hooks one by one meant more than a dozen
// process-wide freezes spread across the game's startup, competing with the
// game's own loading and with whatever else is being injected into the process
// at the same time. MH_ApplyQueued does one freeze for all of them.
bool ApplyQueuedHooks();

// Walks the module's export table directly. GetProcAddress can be hooked by
// other mods and hand out redirected pointers; hooking those misses callers
// that resolved the real export through their import tables.
void* ResolveExportFromTable(HMODULE module, const char* name);

// Hooks one exported function, preferring the address found in the export
// table over the one GetProcAddress reports.
bool InstallExportHook(HMODULE module, const char* name, void* detour,
                       void** original);

// Hooks a fixed address after verifying the bytes there, so an unexpected
// executable is left alone instead of being patched blindly.
bool InstallSignatureHook(uintptr_t address, const unsigned char* signature,
                          size_t signatureSize, void* detour, void** original,
                          const char* name);

}  // namespace bm
