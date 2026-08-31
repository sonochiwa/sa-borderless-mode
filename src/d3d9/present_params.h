#pragma once

#include <d3d9.h>

namespace bm {

enum ConvertMode {
    ConvertNone = 0,
    ConvertFullscreen,
    ConvertVsyncOnly,
};

const char* ConvertModeName(ConvertMode mode);

void LogPresentParams(const char* label, const D3DPRESENT_PARAMETERS* params);

// Fills `converted` with a fixed-up copy of the caller's parameters. The
// caller's struct is never written: for GTA SA it is a persistent global
// (0x00C9C040) that other mods read and write, so in-place edits leak into
// their logic and can re-trigger their reset handling.
ConvertMode ConvertPresentParams(const D3DPRESENT_PARAMETERS* source,
                                 D3DPRESENT_PARAMETERS* converted);

bool Is16BitFormat(D3DFORMAT format);

// Remembers the parameters a successful CreateDevice/Reset was made with, so
// a later identical Reset can be recognized as redundant.
void RememberAppliedParams(const D3DPRESENT_PARAMETERS* params);

// True when the device already presents exactly these parameters and is in a
// healthy cooperative state.
bool IsRedundantReset(IDirect3DDevice9* device,
                      const D3DPRESENT_PARAMETERS* params);

}  // namespace bm
