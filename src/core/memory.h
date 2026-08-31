#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace bm {

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
        kAlreadyApplied,      // every site already carries the patched bytes
        kSignatureMismatch,   // a site matches neither original nor patched
        kProtectFailed,       // VirtualProtect refused the range
        kThreadInRange,       // another thread is executing the patch range
        kWriteFailed,         // the write itself raised an exception
        kApplied,
    };

    Status status;
    uintptr_t mismatchAddress;  // valid for kSignatureMismatch
    size_t written;             // sites written, valid for kApplied
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
