#include "core/hook.h"

#include "core/log.h"
#include "core/memory.h"
#include "MinHook.h"

#include <cstring>

namespace bm {

bool InstallHook(void* target, void* detour, void** original, const char* name) {
    if (!target) {
        Log("%s hook skipped: null target", name);
        return false;
    }

    MH_STATUS create = MH_CreateHook(target, detour, original);
    MH_STATUS queue = create == MH_OK ? MH_QueueEnableHook(target) : create;
    Log("%s hook: target=0x%p create=%d queue=%d original=0x%p",
        name, target, create, queue, *original);
    if (create == MH_OK && queue == MH_OK) {
        return true;
    }

    *original = nullptr;
    return false;
}

bool ApplyQueuedHooks() {
    MH_STATUS status = MH_ApplyQueued();
    Log("queued hooks applied: status=%d", status);
    return status == MH_OK;
}

void* ResolveExportFromTable(HMODULE module, const char* name) {
    __try {
        const BYTE* base = reinterpret_cast<const BYTE*>(module);
        const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return nullptr;
        }
        const IMAGE_NT_HEADERS* nt =
            reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return nullptr;
        }
        const IMAGE_DATA_DIRECTORY& dir =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress || !dir.Size) {
            return nullptr;
        }
        const IMAGE_EXPORT_DIRECTORY* exports =
            reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const DWORD* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const WORD* ordinals =
            reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const DWORD* functions =
            reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i) {
            if (std::strcmp(reinterpret_cast<const char*>(base + names[i]), name) != 0) {
                continue;
            }
            DWORD rva = functions[ordinals[i]];
            if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size) {
                return nullptr;  // forwarded export
            }
            return const_cast<BYTE*>(base) + rva;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

bool InstallExportHook(HMODULE module, const char* name, void* detour,
                       void** original) {
    void* fromGetProc = reinterpret_cast<void*>(GetProcAddress(module, name));
    void* fromTable = ResolveExportFromTable(module, name);
    void* target = fromTable ? fromTable : fromGetProc;
    if (!target) {
        Log("%s hook skipped: export not found", name);
        return false;
    }
    if (fromGetProc && fromGetProc != target) {
        char targetText[kCallerTextSize] = {};
        char getProcText[kCallerTextSize] = {};
        FormatCallerAddress(target, targetText, sizeof(targetText));
        FormatCallerAddress(fromGetProc, getProcText, sizeof(getProcText));
        Log("%s export mismatch: table=%s GetProcAddress=%s (hooking table address)",
            name, targetText, getProcText);
    }
    return InstallHook(target, detour, original, name);
}

bool InstallSignatureHook(uintptr_t address, const unsigned char* signature,
                          size_t signatureSize, void* detour, void** original,
                          const char* name) {
    void* target = reinterpret_cast<void*>(address);
    if (!BytesMatch(target, signature, signatureSize)) {
        Log("%s hook skipped: signature mismatch at 0x%08lX", name,
            static_cast<unsigned long>(address));
        return false;
    }
    return InstallHook(target, detour, original, name);
}

}  // namespace bm
