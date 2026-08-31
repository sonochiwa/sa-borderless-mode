#include "d3d9/device_hooks.h"

#include "core/hook.h"
#include "core/log.h"
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
int g_suppressedRedundantResetCount = 0;

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
        Log("reset hook skipped: existing=%d device=0x%p",
            g_originalReset ? 1 : 0, device);
        return;
    }

    void** vtable = *reinterpret_cast<void***>(device);
    if (InstallHook(vtable[kVtableReset], reinterpret_cast<void*>(&HookedReset),
                    reinterpret_cast<void**>(&g_originalReset), "Reset")) {
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
        Log("after CreateDevice: device=0x%p window=0x%p focus=0x%p mode=%s",
            device, window, focusWindow, ConvertModeName(mode));
        RequireNextReset("CreateDevice");
        InstallWindowHook(window);
        InstallGetMessageHook(window);
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(window);
            if (BorderlessPending()) {
                // The game has not shown its window yet. Wait for it rather
                // than forcing it on screen from inside CreateDevice.
                ScheduleBorderlessRetry(window);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("after CreateDevice failed with exception");
    }
}

void AfterReset(IDirect3DDevice9* device,
                D3DPRESENT_PARAMETERS* params,
                ConvertMode mode) {
    __try {
        Log("after Reset: device=0x%p window=0x%p mode=%s",
            device, GetDeviceWindow(device, params, nullptr),
            ConvertModeName(mode));
        if (mode == ConvertFullscreen) {
            ApplyBorderlessStyle(GetDeviceWindow(device, params, nullptr));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("after Reset failed with exception");
    }
}

HRESULT WINAPI HookedReset(IDirect3DDevice9* device,
                           D3DPRESENT_PARAMETERS* params) {
    void* caller = _ReturnAddress();

    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective =
        mode == ConvertNone ? params : &converted;

    bool redundant = IsRedundantReset(device, effective);
    if (redundant && g_haveForwardedReset) {
        ++g_suppressedRedundantResetCount;
        if (g_suppressedRedundantResetCount <= 10 ||
            (g_suppressedRedundantResetCount % 100) == 0) {
            char callerText[kCallerTextSize] = {};
            FormatCallerAddress(caller, callerText, sizeof(callerText));
            Log("Reset return: suppressed redundant D3D_OK count=%d caller=%s",
                g_suppressedRedundantResetCount, callerText);
        }
        return D3D_OK;
    }

    char callerText[kCallerTextSize] = {};
    FormatCallerAddress(caller, callerText, sizeof(callerText));
    Log("Reset enter: device=0x%p params=0x%p caller=%s", device, params,
        callerText);
    LogPresentParams("Reset input", params);
    if (mode != ConvertNone) {
        LogPresentParams("Reset converted", &converted);
    }
    Log("Reset conversion mode=%s", ConvertModeName(mode));

    if (redundant) {
        Log("Reset redundant-looking request: forwarding first required Reset");
    }

    HRESULT result = g_originalReset(device, effective);
    g_suppressedRedundantResetCount = 0;
    Log("Reset original result=0x%08lX", result);

    D3DPRESENT_PARAMETERS compatible = {};
    if (FAILED(result) && mode == ConvertFullscreen &&
        Is16BitFormat(converted.BackBufferFormat)) {
        compatible = converted;
        compatible.BackBufferFormat = D3DFMT_X8R8G8B8;
        LogPresentParams("Reset 16-bit compatible retry", &compatible);
        result = g_originalReset(device, &compatible);
        Log("Reset 16-bit compatible result=0x%08lX", result);
        if (SUCCEEDED(result)) {
            effective = &compatible;
        }
    }

    if (FAILED(result) && mode != ConvertNone) {
        LogPresentParams("Reset fallback input", params);
        result = g_originalReset(device, params);
        Log("Reset fallback result=0x%08lX", result);
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

    Log("Reset return: final result=0x%08lX mode=%s", result,
        ConvertModeName(mode));
    return result;
}

HRESULT WINAPI HookedCreateDevice(IDirect3D9* self,
                                  UINT adapter,
                                  D3DDEVTYPE deviceType,
                                  HWND focusWindow,
                                  DWORD behaviorFlags,
                                  D3DPRESENT_PARAMETERS* params,
                                  IDirect3DDevice9** device) {
    Log("CreateDevice enter: self=0x%p adapter=%u type=%u focus=0x%p "
        "behavior=0x%08lX params=0x%p out=0x%p",
        self, adapter, deviceType, focusWindow, behaviorFlags, params, device);
    LogPresentParams("CreateDevice input", params);
    D3DPRESENT_PARAMETERS converted = {};
    ConvertMode mode = ConvertPresentParams(params, &converted);
    D3DPRESENT_PARAMETERS* effective = mode == ConvertNone ? params : &converted;
    if (mode != ConvertNone) {
        LogPresentParams("CreateDevice converted", &converted);
    }
    Log("CreateDevice conversion mode=%s", ConvertModeName(mode));

    HRESULT result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                            behaviorFlags, effective, device);
    Log("CreateDevice original result=0x%08lX device=0x%p",
        result, device ? *device : nullptr);

    D3DPRESENT_PARAMETERS compatible = {};
    if (FAILED(result) && mode == ConvertFullscreen &&
        Is16BitFormat(converted.BackBufferFormat)) {
        compatible = converted;
        compatible.BackBufferFormat = D3DFMT_X8R8G8B8;
        LogPresentParams("CreateDevice 16-bit compatible retry", &compatible);
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, &compatible, device);
        Log("CreateDevice 16-bit compatible result=0x%08lX device=0x%p",
            result, device ? *device : nullptr);
        if (SUCCEEDED(result)) {
            effective = &compatible;
        }
    }

    if (FAILED(result) && mode != ConvertNone) {
        LogPresentParams("CreateDevice fallback input", params);
        result = g_originalCreateDevice(self, adapter, deviceType, focusWindow,
                                        behaviorFlags, params, device);
        Log("CreateDevice fallback result=0x%08lX device=0x%p",
            result, device ? *device : nullptr);
        mode = ConvertNone;
        effective = params;
    }

    if (SUCCEEDED(result) && device && *device) {
        RememberAppliedParams(effective);
        AfterCreateDevice(*device, effective, focusWindow, mode);
    }

    Log("CreateDevice return: result=0x%08lX device=0x%p",
        result, device ? *device : nullptr);
    return result;
}

void HookCreateDevice(IDirect3D9* d3d) {
    if (!d3d || InterlockedCompareExchange(&g_createDeviceHookState, 1, 0) != 0) {
        Log("CreateDevice hook skipped: d3d=0x%p state=%ld",
            d3d, g_createDeviceHookState);
        return;
    }

    __try {
        void** vtable = *reinterpret_cast<void***>(d3d);
        if (InstallHook(vtable[kVtableCreateDevice],
                        reinterpret_cast<void*>(&HookedCreateDevice),
                        reinterpret_cast<void**>(&g_originalCreateDevice),
                        "CreateDevice") &&
            ApplyQueuedHooks()) {
            // The game creates its device immediately after Direct3DCreate9
            // returns, so this one cannot wait for a batch either.
            return;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("CreateDevice hook failed with exception");
    }

    InterlockedExchange(&g_createDeviceHookState, 0);
    g_originalCreateDevice = nullptr;
}

IDirect3D9* WINAPI HookedDirect3DCreate9(UINT sdkVersion) {
    Log("Direct3DCreate9 enter: sdk=%u", sdkVersion);
    IDirect3D9* d3d = g_originalDirect3DCreate9(sdkVersion);
    Log("Direct3DCreate9 result=0x%p", d3d);
    HookCreateDevice(d3d);
    return d3d;
}

void TryHookCreateDeviceThroughTemporaryObject(Direct3DCreate9Fn createD3D9) {
    __try {
        IDirect3D9* d3d = createD3D9(D3D_SDK_VERSION);
        Log("temporary D3D9 object=0x%p", d3d);
        if (d3d) {
            HookCreateDevice(d3d);
            d3d->Release();
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("temporary D3D9 hook attempt failed with exception");
    }
}

}  // namespace

void RequireNextReset(const char* reason) {
    g_haveForwardedReset = false;
    Log("next Reset will be forwarded: %s", reason);
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
            Log("d3d9 LoadLibrary failed: error=%lu", GetLastError());
            return;
        }
        Log("d3d9 loaded on demand: module=0x%p", d3d9);
    } else {
        Log("d3d9 already loaded: module=0x%p", d3d9);
    }

    auto createD3D9 = reinterpret_cast<Direct3DCreate9Fn>(
        GetProcAddress(d3d9, "Direct3DCreate9"));
    if (!createD3D9) {
        Log("Direct3DCreate9 export not found");
        return;
    }
    Log("Direct3DCreate9 export=0x%p", reinterpret_cast<void*>(createD3D9));

    if (InstallHook(reinterpret_cast<void*>(createD3D9),
                    reinterpret_cast<void*>(&HookedDirect3DCreate9),
                    reinterpret_cast<void**>(&g_originalDirect3DCreate9),
                    "Direct3DCreate9")) {
        return;
    }

    // Only worth the risk when the export hook is unavailable. Creating a
    // D3D9 object here means running the display-driver and AppCompat
    // (DWM8And16BitMitigation) initialization paths on this thread while
    // the game thread may be running the very same code, and releasing
    // the object can unload the driver DLL underneath it. With the export
    // hook in place every future Direct3DCreate9 comes through us anyway.
    Log("Direct3DCreate9 hook unavailable: falling back to a temporary "
        "D3D9 object");
    TryHookCreateDeviceThroughTemporaryObject(createD3D9);
}

}  // namespace bm
