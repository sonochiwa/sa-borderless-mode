#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace bm {

// Held around every suspension of all other threads: this plugin's own
// (ApplyAtomicPatchSet) and MinHook's (ApplyQueuedHooks). Two plugins that
// suspend all threads at once from different threads suspend each other and
// the game hangs before its window appears; this plugin and Audio Runtime
// did so in about one start in thirty right after a plugin was copied. The
// mutex is named per process, so every plugin that takes it around its
// freezes waits for the others. A holder can itself be suspended for a
// moment by a plugin that does not take it, so the wait gives up after a
// while and the freeze goes ahead.
class ThreadFreezeLock {
public:
    ThreadFreezeLock();
    ~ThreadFreezeLock();
    ThreadFreezeLock(const ThreadFreezeLock&) = delete;
    ThreadFreezeLock& operator=(const ThreadFreezeLock&) = delete;

private:
    HANDLE mutex;
    bool owned;
};

// memcmp that treats an unreadable address as "no match" instead of crashing.
bool BytesMatch(const void* address, const unsigned char* expected, size_t size);

// One byte-level code patch: its address, the bytes expected before patching
// and the bytes written over them.
struct BytePatch {
    uintptr_t address;
    unsigned char original[8];
    unsigned char patched[8];
    size_t size;
};

struct PatchSetResult {
    enum Status {
        // Every site already carries the patched bytes.
        kAlreadyApplied,
        // A site matches neither the original nor the patched bytes.
        kSignatureMismatch,
        // VirtualProtect refused the range.
        kProtectFailed,
        // Another thread is executing the patch range.
        kThreadInRange,
        // The write itself raised an exception.
        kWriteFailed,
        kApplied,
    };

    Status status;
    // Valid for kSignatureMismatch.
    uintptr_t mismatchAddress;
    // Sites written, valid for kApplied.
    size_t written;
    bool frozeThreads;
    unsigned frozenThreads;
};

// Writes a set of patches that are only meaningful together, in one pass with
// every other thread in the process suspended. A thread that executes the
// function while only part of the set has landed can run with an unbalanced
// stack and take the process down, which is why this is not a sequence of
// independent writes.
//
// Nothing between the freeze and the matching resume may log, allocate or
// touch the loader: a suspended thread can be holding the log lock, a heap
// lock or the loader lock, and waiting on any of them here deadlocks the
// process. The result is therefore reported back to the caller to log.
PatchSetResult ApplyAtomicPatchSet(const BytePatch* patches, size_t count,
                                   uintptr_t rangeBegin, uintptr_t rangeEnd);

}  // namespace bm
