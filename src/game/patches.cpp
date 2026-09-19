#include "game/patches.h"

#include "core/hook.h"
#include "core/memory.h"
#include "game/addresses.h"

#include <windows.h>

namespace bm {
namespace {

using ApplyVideoModeFn = int (__cdecl*)(void* arg1, void* arg2, void* arg3);

ApplyVideoModeFn g_originalApplyVideoMode = nullptr;

// The three sites are only meaningful together: the first turns a call into a
// short jump over a block, the last removes the matching `pop esi`. A thread
// that executes the function while only part of the set has landed runs with
// an unbalanced stack and takes the process down - which is why the whole set
// is written in one pass with every other thread stopped, rather than as
// three independent writes.
constexpr BytePatch kNoFrameDelayPatches[] = {
    { 0x0053E923, { 0xE8, 0x58 }, { 0xEB, 0x43 }, 2 },
    { 0x0053E99F, { 0x14 },       { 0x10 },       1 },
    { 0x0053E9A5, { 0x5E },       { 0x90 },       1 },
};
constexpr size_t kNoFrameDelayPatchCount =
    sizeof(kNoFrameDelayPatches) / sizeof(kNoFrameDelayPatches[0]);

// Returns 1 when the flag was clear and has been set, 0 when it already was
// set, -1 when the access faulted. Kept apart from its caller so the caller is
// free to use anything __try would not allow.
int SetGameInFocusFlag() {
    DWORD* flag = reinterpret_cast<DWORD*>(game::kGameInFocus);
    __try {
        if (*flag != 0) {
            return 0;
        }
        *flag = 1;
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int __cdecl HookedApplyVideoMode(void* arg1, void* arg2, void* arg3) {
    // Matches RefreshRateFixByDarkP1xel32: GTA reads this global while
    // rebuilding the selected video mode.
    UpdateGameRefreshRate();
    ApplyNoFrameDelay();
    int result = g_originalApplyVideoMode(arg1, arg2, arg3);
    // Other patches may restore the original frame-delay bytes while video
    // resources are rebuilt. Make our change persistent across mode switches.
    ApplyNoFrameDelay();
    return result;
}

}  // namespace

void ApplyNoFrameDelay() {
    if (!game::IsSupportedExecutable()) {
        return;
    }

    const PatchSetResult result =
        ApplyAtomicPatchSet(kNoFrameDelayPatches, kNoFrameDelayPatchCount,
                            game::kNoFrameDelayRangeBegin,
                            game::kNoFrameDelayRangeEnd);

    switch (result.status) {
        case PatchSetResult::kAlreadyApplied:
            break;
        case PatchSetResult::kApplied:
            break;
        case PatchSetResult::kSignatureMismatch:
            break;
        case PatchSetResult::kProtectFailed:
            break;
        case PatchSetResult::kThreadInRange:
            // A later call retries; the game is left running unpatched
            // meanwhile, which is correct behaviour rather than a
            // half-applied patch.
            break;
        case PatchSetResult::kWriteFailed:
            break;
    }
}

bool UpdateGameRefreshRate() {
    if (!game::IsSupportedExecutable()) {
        return false;
    }

    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsExW(nullptr, ENUM_CURRENT_SETTINGS, &mode, 0) ||
        mode.dmDisplayFrequency <= 1) {
        return false;
    }

    DWORD* gameRefreshRate = reinterpret_cast<DWORD*>(game::kRefreshRate);
    __try {
        *gameRefreshRate = mode.dmDisplayFrequency;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void RestoreGameInFocus() {
    if (!game::IsSupportedExecutable()) {
        return;
    }

    static bool loggedMismatch = false;
    if (!BytesMatch(reinterpret_cast<const void*>(game::kGameInFocusClearSite),
                    game::kGameInFocusClearSignature,
                    sizeof(game::kGameInFocusClearSignature))) {
        if (!loggedMismatch) {
            loggedMismatch = true;
        }
        return;
    }

    const int result = SetGameInFocusFlag();
    if (result > 0) {
    } else if (result < 0) {
    }
}

void HookApplyVideoMode() {
    if (!game::IsSupportedExecutable()) {
        return;
    }

    InstallSignatureHook(game::kApplyVideoMode,
                         game::kApplyVideoModeSignature,
                         sizeof(game::kApplyVideoModeSignature),
                         reinterpret_cast<void*>(&HookedApplyVideoMode),
                         reinterpret_cast<void**>(&g_originalApplyVideoMode));
}

}  // namespace bm
