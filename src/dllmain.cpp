// BorderlessMode.asi - borderless fullscreen windowed mode for GTA San
// Andreas without capping FPS at the monitor refresh rate.
//
// Layout of the sources:
//   core/    logging, configuration, module paths, hook and patch helpers
//   d3d9/    present-parameter conversion and the D3D9 device hooks
//   window/  borderless geometry, the window procedure, display-mode guard
//   input/   key-state, message-pump and cursor filters
//   game/    everything that depends on GTA SA 1.0 US addresses

#include "core/config.h"
#include "core/log.h"
#include "core/module.h"
#include "d3d9/device_hooks.h"
#include "game/fps_overlay.h"
#include "game/patches.h"
#include "input/cursor.h"
#include "input/key_filter.h"
#include "input/message_pump.h"
#include "window/display_mode.h"

#include <windows.h>

#include "MinHook.h"

namespace {

DWORD WINAPI Initialize(LPVOID) {
    bm::LoadConfig();
    bm::LogOpen();
    bm::Log("initialize begin: module=0x%p log=%d",
            bm::SelfModule(), bm::LogEnabled() ? 1 : 0);

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        bm::Log("MinHook initialize failed: status=%d", status);
        return 0;
    }
    bm::Log("MinHook initialized: status=%d", status);

    // NoFrameDelay is deliberately not applied here. This runs on a thread
    // spawned from DllMain, in parallel with the game's own startup, so the
    // patch target may be executing. It is applied from the ApplyVideoMode
    // and CreateDevice hooks instead, both of which run on the game thread at
    // a point where the patched function is not on any stack.
    bm::CaptureDesktopMode();
    bm::UpdateGameRefreshRate();
    bm::HookApplyVideoMode();
    bm::HookFrameOutput();
    bm::HookChangeDisplaySettings();
    bm::InstallD3D9Hooks();
    bm::HookSetCursorPos();
    bm::HookKeyStateApis();
    bm::HookMessagePump();
    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        // Shutdown ordering matters here. The hooks are still installed and
        // other threads can be inside them, so nothing they depend on may be
        // destroyed: a thread that passed the guard in Log() a moment ago is
        // about to enter the log lock. Destroying the critical section here
        // made Rtl*CriticalSection raise STATUS_INVALID_PARAMETER (0xC000000D)
        // and took the process down on the way out. The flag stops new work;
        // the lock and the log handle are deliberately left for the OS to
        // reclaim at process exit.
        bm::LogShutdown();
        bm::RemoveGetMessageHook();
        return TRUE;
    }

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    bm::SetSelfModule(instance);
    DisableThreadLibraryCalls(instance);
    bm::PinSelf();
    bm::LogInit();

    HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        bm::LogOpen();
        bm::Log("initialize thread create failed: error=%lu", GetLastError());
    }

    return TRUE;
}
