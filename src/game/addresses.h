#pragma once

#include <windows.h>

#include <cstdint>

// Every hard-coded GTA SA address lives here. All of them are GTA SA 1.0 US
// and are only used after IsSupportedExecutable() confirms the image base and
// the call sites verify the expected bytes, so an unknown executable simply
// loses the address-dependent features instead of being patched blindly.
namespace bm::game {

constexpr uintptr_t kImageBase = 0x00400000;

// CCamera::ShowRaster - GTA's final frame-output call. Hooked instead of
// D3D9 Present, which must stay untouched for third-party overlays.
constexpr uintptr_t kShowRaster = 0x00745240;
constexpr unsigned char kShowRasterSignature[] = {
    0xA0, 0x94, 0x67, 0xBA, 0x00, 0x84, 0xC0
};

// Frame counter advanced once per game frame; the FPS sample is taken from it.
constexpr uintptr_t kFrameCounter = 0x00B7CB4C;

// CMessages::AddMessageJumpQ - the text box the game uses for its own
// notices. Confirms the FPS counter command on screen.
constexpr uintptr_t kAddMessageJumpQ = 0x0069F1E0;
constexpr unsigned char kAddMessageJumpQSignature[] = {
    0x81, 0xEC, 0x20, 0x03, 0x00, 0x00, 0x56
};

// The game's IDirect3DDevice9*.
constexpr uintptr_t kDirect3DDevice = 0x00C97C28;

// GTA's "the window has input focus" flag. While it is zero WinMain does
// nothing but pump messages and sleep 100 ms per iteration: no rendering, no
// input, no game logic. Stock GTA sets it back to 1 from two places, the
// WM_ACTIVATE and the WM_SETFOCUS arms of its window procedure.
constexpr uintptr_t kGameInFocus = 0x008D621C;

// The WM_KILLFOCUS arm of the same window procedure - `mov [kGameInFocus], 0`.
// It is only read, never written, and only to confirm that the flag really
// does live at kGameInFocus on this executable. The WM_ACTIVATE arm would be
// the more obvious anchor and is deliberately not used: SAMPGraphicRestore.asi
// overwrites that one with NOPs, and surviving exactly that is the point.
constexpr uintptr_t kGameInFocusClearSite = 0x00748054;
constexpr unsigned char kGameInFocusClearSignature[] = {
    0xC7, 0x05, 0x1C, 0x62, 0x8D, 0x00, 0x00, 0x00, 0x00, 0x00
};

// Refresh rate the game rebuilds its selected video mode from.
constexpr uintptr_t kRefreshRate = 0x008E243C;

// The function that applies the selected video mode.
constexpr uintptr_t kApplyVideoMode = 0x007F6CB0;
constexpr unsigned char kApplyVideoModeSignature[] = {
    0x8A, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x10
};

// The frame-delay block rewritten by the NoFrameDelay patch.
constexpr uintptr_t kNoFrameDelayRangeBegin = 0x0053E923;
constexpr uintptr_t kNoFrameDelayRangeEnd = 0x0053E9A6;

// The game's persistent D3DPRESENT_PARAMETERS (0x00C9C040) is deliberately
// never written by this plugin: other mods read and write it, so in-place
// edits leak into their logic.

inline bool IsSupportedExecutable() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) == kImageBase;
}

}  // namespace bm::game
