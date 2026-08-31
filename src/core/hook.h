#pragma once

#include <windows.h>

#include <cstddef>

namespace bm {

// Creates and enables a MinHook detour, logging the outcome under `name`.
// `*original` is cleared when the hook could not be installed, so callers can
// treat a null trampoline as "feature unavailable".
bool InstallHook(void* target, void* detour, void** original, const char* name);

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
