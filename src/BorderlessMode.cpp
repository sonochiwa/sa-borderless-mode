// The game creates an exclusive fullscreen Direct3D 9 device, which ties
// its frame rate to the monitor refresh, makes every Alt-Tab a mode switch
// and confuses overlays and capture tools. The plugin converts the device's
// present parameters to a windowed swap chain, restyles the game window into
// a borderless one covering the monitor, and then keeps the game from
// noticing: display-mode changes, cursor confinement, focus-driven key state
// and the message pump are filtered so the game behaves as it would have in
// exclusive mode, while its own frame delay is patched out so the FPS is no
// longer capped at the refresh rate.

#include "core/hook.h"
#include "core/module.h"
#include "d3d9/device_hooks.h"
#include "game/patches.h"
#include "input/cursor.h"
#include "input/key_filter.h"
#include "input/message_pump.h"
#include "window/display_mode.h"

#include <windows.h>

#include "MinHook.h"

namespace {

DWORD WINAPI Initialize(LPVOID) {
    bm::MakeProcessDpiAware();

    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        return 0;
    }

    // NoFrameDelay is deliberately not applied here. This runs on a thread
    // spawned from DllMain, in parallel with the game's own startup, so the
    // patch target may be executing. It is applied from the ApplyVideoMode
    // and CreateDevice hooks instead, both of which run on the game thread at
    // a point where the patched function is not on any stack.
    bm::CaptureDesktopMode();
    bm::UpdateGameRefreshRate();

    // Every Hook* call below only creates and queues its detours; none of them
    // touches another thread. They are activated together by the single
    // ApplyQueuedHooks() at the end, so the game's startup is interrupted by
    // one process-wide thread freeze instead of one per hook. The game reaches
    // Direct3DCreate9 while this runs, so keep the stretch between the first
    // create and the apply free of anything slow.
    bm::HookApplyVideoMode();
    bm::HookChangeDisplaySettings();
    bm::InstallD3D9Hooks();
    bm::HookSetCursorPos();
    bm::HookKeyStateApis();
    bm::HookMessagePump();
    bm::ApplyQueuedHooks();

    return 0;
}

}  // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        // The hooks are still installed and other threads can be inside
        // them, so nothing they depend on is destroyed here.
        bm::RemoveGetMessageHook();
        return TRUE;
    }

    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }

    bm::SetSelfModule(instance);
    DisableThreadLibraryCalls(instance);
    bm::PinSelf();

    HANDLE thread = CreateThread(nullptr, 0, Initialize, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }

    return TRUE;
}
