#include "d3d9/present_params.h"

namespace bm {
namespace {

D3DPRESENT_PARAMETERS g_appliedParams = {};
bool g_haveAppliedParams = false;

bool SameDisplayMode(const D3DPRESENT_PARAMETERS& a,
                     const D3DPRESENT_PARAMETERS& b) {
    return a.Windowed == b.Windowed &&
           a.BackBufferWidth == b.BackBufferWidth &&
           a.BackBufferHeight == b.BackBufferHeight &&
           a.BackBufferFormat == b.BackBufferFormat &&
           a.BackBufferCount == b.BackBufferCount &&
           a.MultiSampleType == b.MultiSampleType &&
           a.SwapEffect == b.SwapEffect &&
           a.hDeviceWindow == b.hDeviceWindow &&
           a.EnableAutoDepthStencil == b.EnableAutoDepthStencil &&
           a.AutoDepthStencilFormat == b.AutoDepthStencilFormat &&
           a.FullScreen_RefreshRateInHz == b.FullScreen_RefreshRateInHz &&
           a.PresentationInterval == b.PresentationInterval;
}

void ApplyWindowedPresentParams(D3DPRESENT_PARAMETERS* params) {
    params->Windowed = TRUE;
    params->FullScreen_RefreshRateInHz = 0;
    params->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    params->SwapEffect = D3DSWAPEFFECT_DISCARD;
}

}  // namespace

ConvertMode ConvertPresentParams(const D3DPRESENT_PARAMETERS* source,
                                 D3DPRESENT_PARAMETERS* converted) {
    __try {
        if (!source) {
            return ConvertNone;
        }

        if (source->Windowed) {
            if (source->PresentationInterval == D3DPRESENT_INTERVAL_IMMEDIATE &&
                source->FullScreen_RefreshRateInHz == 0) {
                return ConvertNone;
            }

            *converted = *source;
            converted->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
            // Windowed devices must not carry a fullscreen refresh rate.
            // Mods such as GameTweaker keep writing one into the game's
            // global present params, which real d3d9 rejects when windowed.
            converted->FullScreen_RefreshRateInHz = 0;
            return ConvertVsyncOnly;
        }

        *converted = *source;
        ApplyWindowedPresentParams(converted);
        return ConvertFullscreen;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ConvertNone;
    }
}

bool Is16BitFormat(D3DFORMAT format) {
    return format == D3DFMT_R5G6B5 ||
           format == D3DFMT_X1R5G5B5 ||
           format == D3DFMT_A1R5G5B5;
}

void RememberAppliedParams(const D3DPRESENT_PARAMETERS* params) {
    __try {
        if (params) {
            g_appliedParams = *params;
            g_haveAppliedParams = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsRedundantReset(IDirect3DDevice9* device,
                      const D3DPRESENT_PARAMETERS* params) {
    if (!device || !params || !g_haveAppliedParams) {
        return false;
    }
    __try {
        HRESULT cooperative = device->TestCooperativeLevel();
        return SameDisplayMode(*params, g_appliedParams) &&
               cooperative == D3D_OK;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace bm
