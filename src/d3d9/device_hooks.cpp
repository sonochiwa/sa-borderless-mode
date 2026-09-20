#include "d3d9/device_hooks.h"

#include "core/hook.h"
#include "d3d9/present_params.h"
#include "game/patches.h"
#include "input/message_pump.h"
#include "window/borderless.h"
#include "window/window_proc.h"

#include <intrin.h>
#include <windows.h>

namespace bm {
namespace {

// IDirect3D9::CreateDevice and IDirect3DDevice9::Reset happen to share the
// same vtable slot in their respective interfaces.
constexpr int kVtableCreateDevice = 16;
constexpr int kVtableReset = 16;

using Direct3DCreate9Fn = IDirect3D9* (WINAPI*)(UINT sdkVersion);
using CreateDeviceFn = HRESULT (WINAPI*)(IDirect3D9* self, UINT adapter,
                                         D3DDEVTYPE deviceType, HWND focusWindow,
                                         DWORD behaviorFlags,
                                         D3DPRESENT_PARAMETERS* params,
                                         IDirect3DDevice9** device);
using ResetFn = HRESULT (WINAPI*)(IDirect3DDevice9* self,
                                  D3DPRESENT_PARAMETERS* params);

Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
CreateDeviceFn g_originalCreateDevice = nullptr;
ResetFn g_originalReset = nullptr;

LONG g_createDeviceHookState = 0;
bool g_haveForwardedReset = false;

// The device PresentIdleFrame() presents, and the window it presents to. The
// plugin holds its own reference: GTA destroys and rebuilds its window and
// device while it settles on a video mode, and a pointer taken from
// CreateDevice would otherwise dangle until the next CreateDevice replaces
// it. Only windowed devices are held; keeping an exclusive one alive would
// stop the game from creating its replacement.
IDirect3DDevice9* g_idleDevice = nullptr;
HWND g_idleWindow = nullptr;

void TrackIdleDevice(IDirect3DDevice9* device, HWND window) {
    if (!device || device == g_idleDevice) {
        g_idleWindow = window;
        return;
    }
    ReleaseIdleDevice();
    __try {
        device->AddRef();
        g_idleDevice = device;
        g_idleWindow = window;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_idleDevice = nullptr;
        g_idleWindow = nullptr;
    }
}

HWND GetDeviceWindow(IDirect3DDevice9* device,
                     D3DPRESENT_PARAMETERS* params,
                     HWND focusWindow) {
    if (params && params->hDeviceWindow) {
        return params->hDeviceWindow;
    }

    D3DDEVICE_CREATION_PARAMETERS creation = {};
    if (device &&
        SUCCEEDED(device->GetCreationParameters(&creation)) &&
        creation.hFocusWindow) {
        return creation.hFocusWindow;
    }

    return focusWindow;
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device,
                           D3DPRESENT_PARAMETERS* params);

void HookDeviceReset(IDirect3DDevice9* device) {
    if (g_originalReset || !device) {
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (InstallHook(vtable[kVtableReset], reinterpret_cast<void*>(&HookedReset),
                    reinterpret_cast<void**>(&g_originalReset))) {
        // Installed from the game thread once the device exists: the very next
        // Reset must already go through us, so it cannot wait for a batch.
        ApplyQueuedHooks();
    }
}

void AfterCreateDevice(IDirect3DDevice9* device,
                       D3DPRESENT_PARAMETERS* params,
                       HWND focusWindow,
                       ConvertMode mode) {
    __try {
        // First safe point on the game thread: the video mode has been chosen
        // and the patched function is not on the stack here.
        ApplyNoFrameDelay();
        HookDeviceReset(device);

        HWND window = GetDeviceWindow(device, params, focusWindow);
        RequireNextReset();
        InstallWindowHook(window);
        InstallGetMessageHook(window);
        if (mode != ConvertNone && HookedWindow() == window) {
            TrackIdleDevice(device, window);
        }
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(window);
            if (BorderlessPending()) {
                // The game has not shown its window yet. Wait for it rather
                // than forcing it on screen from inside CreateDevice.
                ScheduleBorderlessRetry(window);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void AfterReset(IDirect3DDevice9* device,
                D3DPRESENT_PARAMETERS* params,
                ConvertMode mode) {
    __try {
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(GetDeviceWindow(device, params, nullptr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device,
                           D3DPRESENT_PARAMETERS* params) {
    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective =
        mode == ConvertNone ? params : &converted;

    bool redundant = IsRedundantReset(device, effective);
    if (redundant && g_haveForwardedReset) {
        return D3D_OK;
    }

    HRESULT result = g_originalReset(device, effective);

    D3DPRESENT_PARAMETERS compatible = {};
    if (FAILED(result) && mode == ConvertFullscreen &&
        Is16BitFormat(converted.BackBufferFormat)) {
        compatible = converted;
        compatible.BackBufferFormat = D3DFMT_X8R8G8B8;
        result = g_originalReset(device, &compatible);
        if (SUCCEEDED(result)) {
            effective = &compatible;
            // Worth saying plainly: the game asked for a 16-bit back buffer,
            // could not have one windowed, and is now running at 32-bit. That
            // changes how the game looks, and the log is the only place anyone
            // would find out why.
        }
    }

    if (FAILED(result) && mode != ConvertNone) {
        result = g_originalReset(device, params);
        mode = ConvertNone;
        effective = params;
    }

    if (SUCCEEDED(result)) {
        RememberAppliedParams(effective);
        g_haveForwardedReset = true;
        ApplyNoFrameDelay();
        if (mode != ConvertNone) {
            AfterReset(device, effective, mode);
        }
    }

    return result;
}

HRESULT WINAPI HookedCreateDevice(IDirect3D9* self,
                                  UINT adapter,
                                  D3DDEVTYPE deviceType,
                                  HWND focusWindow,
                                  DWORD behaviorFlags,
                                  D3DPRESENT_PARAMETERS* params,
                                  IDirect3DDevice9** device) {
    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective = mode == ConvertNone ? params : &converted;

    HRESULT result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                            behaviorFlags, effective, device);

    D3DPRESENT_PARAMETERS compatible = {};
    if (FAILED(result) && mode == ConvertFullscreen &&
        Is16BitFormat(converted.BackBufferFormat)) {
        compatible = converted;
        compatible.BackBufferFormat = D3DFMT_X8R8G8B8;
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, &compatible, device);
        if (SUCCEEDED(result)) {
            effective = &compatible;
            // Worth saying plainly: the game asked for a 16-bit back buffer,
            // could not have one windowed, and is now running at 32-bit. That
            // changes how the game looks, and the log is the only place anyone
            // would find out why.
        }
    }

    if (FAILED(result) && mode != ConvertNone) {
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, params, device);
        mode = ConvertNone;
        effective = params;
    }

    if (SUCCEEDED(result) && device && *device) {
        RememberAppliedParams(effective);
        AfterCreateDevice(*device, effective, focusWindow, mode);
    }

    return result;
}

void HookCreateDevice(IDirect3D9* d3d) {
    if (!d3d || InterlockedCompareExchange(&g_createDeviceHookState, 1, 0) != 0) {
        return;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(d3d);
        if (InstallHook(vtable[kVtableCreateDevice],
                        reinterpret_cast<void*>(&HookedCreateDevice),
                        reinterpret_cast<void**>(&g_originalCreateDevice)) &&
            ApplyQueuedHooks()) {
            // The game creates its device immediately after Direct3DCreate9
            // returns, so this one cannot wait for a batch either.
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    InterlockedExchange(&g_createDeviceHookState, 0);
    g_originalCreateDevice = nullptr;
}

IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    HookCreateDevice(d3d);
    return d3d;
}

void TryHookCreateDeviceThroughTemporaryObject(Direct3DCreate9Fn createD3D9) {
    __try {
        IDirect3D9* d3d = createD3D9(D3D_SDK_VERSION);
        if (d3d) {
            HookCreateDevice(d3d);
            d3d->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

void RequireNextReset() {
    g_haveForwardedReset = false;
}

void PresentIdleFrame() {
    if (!g_idleDevice || !g_idleWindow || !IsWindow(g_idleWindow)) {
        return;
    }
    __try {
        if (g_idleDevice->TestCooperativeLevel() != D3D_OK) {
            return;
        }
        // The swap chain's Present rather than the device's: the device's is
        // where SA-MP, CLEO scripts and other mods hook in to draw their
        // interfaces, and the frame being shown again already carries them.
        // A windowed DISCARD chain with one back buffer keeps the last frame
        // in place, so nothing but the presentation itself changes.
        IDirect3DSwapChain9* swapChain = nullptr;
        if (FAILED(g_idleDevice->GetSwapChain(0, &swapChain)) || !swapChain) {
            return;
        }
        swapChain->Present(nullptr, nullptr, nullptr, nullptr, 0);
        swapChain->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void ReleaseIdleDevice() {
    IDirect3DDevice9* device = g_idleDevice;
    g_idleDevice = nullptr;
    g_idleWindow = nullptr;
    if (!device) {
        return;
    }
    __try {
        device->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void InstallD3D9Hooks() {
    // The game imports d3d9.dll statically, so by the time this runs it is
    // already loaded. Take the handle instead of calling LoadLibrary: many
    // modpacks ship a proxy d3d9.dll of their own (ENB, mod_sa and friends),
    // and there is no reason for this plugin to add a reference to it, let
    // alone force one to load that the game itself never asked for.
    HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
    if (!d3d9) {
        d3d9 = LoadLibraryW(L"d3d9.dll");
        if (!d3d9) {
            return;
        }
    } else {
    }

    auto createD3D9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!createD3D9) {
        return;
    }

    if (InstallHook(reinterpret_cast<void*>(createD3D9),
                    reinterpret_cast<void*>(&HookedDirect3DCreate9),
                    reinterpret_cast<void**>(&g_originalDirect3DCreate9))) {
        return;
    }

    // Only worth the risk when the export hook is unavailable. Creating a
    // D3D9 object here means running the display-driver and AppCompat
    // (DWM8And16BitMitigation) initialization paths on this thread while
    // the game thread may be running the very same code, and releasing
    // the object can unload the driver DLL underneath it. With the export
    // hook in place every future Direct3DCreate9 comes through us anyway.
    TryHookCreateDeviceThroughTemporaryObject(createD3D9);
}

}  // namespace bm
