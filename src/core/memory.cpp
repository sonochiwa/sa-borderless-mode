#include "core/memory.h"

#include <tlhelp32.h>

#include <cstring>

namespace bm {
namespace {

constexpr size_t kMaxFrozenThreads = 256;

struct ThreadFreeze {
    HANDLE handles[kMaxFrozenThreads];
    size_t count;
};

// Suspends every other thread in this process.
bool FreezeOtherThreads(ThreadFreeze* freeze) {
    freeze->count = 0;

    const DWORD ourThread = GetCurrentThreadId();
    const DWORD ourProcess = GetCurrentProcessId();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    THREADENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.dwSize < FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) +
                                   sizeof(entry.th32OwnerProcessID)) {
                continue;
            }
            if (entry.th32OwnerProcessID != ourProcess ||
                entry.th32ThreadID == ourThread) {
                continue;
            }
            if (freeze->count >= kMaxFrozenThreads) {
                break;
            }
            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                entry.th32ThreadID);
            if (!thread) {
                continue;
            }
            if (SuspendThread(thread) == static_cast<DWORD>(-1)) {
                CloseHandle(thread);
                continue;
            }
            freeze->handles[freeze->count++] = thread;
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return true;
}

void ResumeOtherThreads(ThreadFreeze* freeze) {
    for (size_t i = 0; i < freeze->count; ++i) {
        ResumeThread(freeze->handles[i]);
        CloseHandle(freeze->handles[i]);
    }
    freeze->count = 0;
}

// True while any frozen thread is stopped inside the code being rewritten.
// Patching underneath it would resume it into a half-rewritten function.
bool AnyThreadInRange(const ThreadFreeze* freeze, uintptr_t begin,
                      uintptr_t end) {
    for (size_t i = 0; i < freeze->count; ++i) {
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(freeze->handles[i], &context)) {
            // An unreadable context is treated as "possibly inside".
            return true;
        }
        uintptr_t ip = static_cast<uintptr_t>(context.Eip);
        if (ip >= begin && ip < end) {
            return true;
        }
    }
    return false;
}

}  // namespace

bool BytesMatch(const void* address, const unsigned char* expected, size_t size) {
    __try {
        return std::memcmp(address, expected, size) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

PatchSetResult ApplyAtomicPatchSet(const BytePatch* patches, size_t count,
                                   uintptr_t rangeBegin, uintptr_t rangeEnd) {
    PatchSetResult result = {};

    size_t alreadyPatched = 0;
    for (size_t i = 0; i < count; ++i) {
        const BytePatch& patch = patches[i];
        const void* address = reinterpret_cast<const void*>(patch.address);
        if (BytesMatch(address, patch.patched, patch.size)) {
            ++alreadyPatched;
            continue;
        }
        if (!BytesMatch(address, patch.original, patch.size)) {
            result.status = PatchSetResult::kSignatureMismatch;
            result.mismatchAddress = patch.address;
            return result;
        }
    }

    if (alreadyPatched == count) {
        result.status = PatchSetResult::kAlreadyApplied;
        return result;
    }

    void* rangeBase = reinterpret_cast<void*>(rangeBegin);
    const SIZE_T rangeSize = static_cast<SIZE_T>(rangeEnd - rangeBegin);
    DWORD oldProtect = 0;
    if (!VirtualProtect(rangeBase, rangeSize, PAGE_EXECUTE_READWRITE,
                        &oldProtect)) {
        result.status = PatchSetResult::kProtectFailed;
        return result;
    }

    ThreadFreeze freeze = {};
    const bool frozen = FreezeOtherThreads(&freeze);
    const bool blocked =
        frozen && AnyThreadInRange(&freeze, rangeBegin, rangeEnd);
    bool wrote = false;

    if (!blocked) {
        __try {
            for (size_t i = 0; i < count; ++i) {
                std::memcpy(reinterpret_cast<void*>(patches[i].address),
                            patches[i].patched, patches[i].size);
            }
            wrote = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            wrote = false;
        }
    }

    result.frozeThreads = frozen;
    result.frozenThreads = static_cast<unsigned>(freeze.count);
    ResumeOtherThreads(&freeze);

    DWORD ignored = 0;
    VirtualProtect(rangeBase, rangeSize, oldProtect, &ignored);
    if (wrote) {
        FlushInstructionCache(GetCurrentProcess(), rangeBase, rangeSize);
        result.status = PatchSetResult::kApplied;
        result.written = count - alreadyPatched;
    } else if (blocked) {
        result.status = PatchSetResult::kThreadInRange;
    } else {
        result.status = PatchSetResult::kWriteFailed;
    }
    return result;
}

}  // namespace bm
