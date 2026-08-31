#include "game/fps_overlay.h"

#include "core/config.h"
#include "core/hook.h"
#include "core/log.h"
#include "game/addresses.h"

#include <d3d9.h>
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace bm {
namespace {

using ShowRasterFn = void (__cdecl*)(void* camera);

ShowRasterFn g_originalShowRaster = nullptr;

// 5x7 bitmap font, one bit per pixel, only the characters the counter needs.
constexpr LONG kGlyphWidth = 5;
constexpr LONG kGlyphHeight = 7;

const unsigned char* FpsGlyph(char character) {
    static const unsigned char space[kGlyphHeight] = {0, 0, 0, 0, 0, 0, 0};
    static const unsigned char f[kGlyphHeight] = {31, 16, 16, 30, 16, 16, 16};
    static const unsigned char p[kGlyphHeight] = {30, 17, 17, 30, 16, 16, 16};
    static const unsigned char s[kGlyphHeight] = {15, 16, 16, 14, 1, 1, 30};
    static const unsigned char digits[10][kGlyphHeight] = {
        {14, 17, 19, 21, 25, 17, 14},
        {4, 12, 4, 4, 4, 4, 14},
        {14, 17, 1, 2, 4, 8, 31},
        {30, 1, 1, 14, 1, 1, 30},
        {2, 6, 10, 18, 31, 2, 2},
        {31, 16, 16, 30, 1, 1, 30},
        {14, 16, 16, 30, 17, 17, 14},
        {31, 1, 2, 4, 8, 8, 8},
        {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 1, 14},
    };

    switch (character) {
        case 'F': return f;
        case 'P': return p;
        case 'S': return s;
        case ' ': return space;
        default:
            if (character >= '0' && character <= '9') {
                return digits[character - '0'];
            }
            return space;
    }
}

void DrawFpsOverlay(IDirect3DDevice9* device, unsigned fps) {
    if (!device || !ShowFpsOverlay()) {
        return;
    }

    char text[24] = {};
    std::snprintf(text, sizeof(text), "%u", fps);

    constexpr LONG scale = 3;
    constexpr LONG glyphAdvance = (kGlyphWidth + 1) * scale;
    const LONG textWidth =
        static_cast<LONG>(std::strlen(text)) * glyphAdvance - scale;

    D3DVIEWPORT9 viewport = {};
    if (FAILED(device->GetViewport(&viewport))) {
        return;
    }

    const LONG originX =
        static_cast<LONG>(viewport.X + viewport.Width) - textWidth - 14;
    const LONG originY = static_cast<LONG>(viewport.Y) + 14;
    constexpr size_t kMaxRects = 640;
    D3DRECT pixels[kMaxRects] = {};
    size_t pixelCount = 0;

    for (size_t glyphIndex = 0; text[glyphIndex] != '\0'; ++glyphIndex) {
        const unsigned char* glyph = FpsGlyph(text[glyphIndex]);
        for (LONG row = 0; row < kGlyphHeight; ++row) {
            for (LONG column = 0; column < kGlyphWidth; ++column) {
                if (!(glyph[row] & (1u << (kGlyphWidth - 1 - column))) ||
                    pixelCount >= kMaxRects) {
                    continue;
                }
                const LONG left =
                    originX + static_cast<LONG>(glyphIndex) * glyphAdvance +
                    column * scale;
                const LONG top = originY + row * scale;
                pixels[pixelCount++] = {
                    left, top, left + scale, top + scale
                };
            }
        }
    }

    D3DRECT background = {
        originX - 6, originY - 6,
        originX + textWidth + 6, originY + kGlyphHeight * scale + 6
    };
    device->Clear(1, &background, D3DCLEAR_TARGET,
                  D3DCOLOR_XRGB(12, 12, 12), 1.0f, 0);
    if (pixelCount) {
        device->Clear(static_cast<DWORD>(pixelCount), pixels, D3DCLEAR_TARGET,
                      D3DCOLOR_XRGB(100, 255, 130), 1.0f, 0);
    }
}

void __cdecl HookedShowRaster(void* camera) {
    static ULONGLONG sampleStart = 0;
    static DWORD sampleStartFrame = 0;
    static unsigned measuredFps = 0;

    __try {
        // GTA advances this counter once per game frame. Reading it here keeps
        // the FPS overlay on GTA's final frame-output path instead of
        // intercepting D3D9 Present, which must remain untouched for
        // third-party overlays. Drawing immediately before ShowRaster also
        // keeps the counter above every HUD element rendered by GTA and SA-MP.
        const DWORD gameFrame = *reinterpret_cast<DWORD*>(game::kFrameCounter);
        const ULONGLONG now = GetTickCount64();
        if (!sampleStart) {
            sampleStart = now;
            sampleStartFrame = gameFrame;
        }
        const ULONGLONG elapsed = now - sampleStart;
        if (elapsed >= 500) {
            const DWORD elapsedFrames = gameFrame - sampleStartFrame;
            measuredFps = static_cast<unsigned>(
                (static_cast<ULONGLONG>(elapsedFrames) * 1000 +
                 elapsed / 2) / elapsed);
            sampleStart = now;
            sampleStartFrame = gameFrame;
        }

        IDirect3DDevice9* device =
            *reinterpret_cast<IDirect3DDevice9**>(game::kDirect3DDevice);
        DrawFpsOverlay(device, measuredFps);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }

    g_originalShowRaster(camera);
}

}  // namespace

void HookFrameOutput() {
    if (g_originalShowRaster) {
        Log("ShowRaster hook skipped: already installed");
        return;
    }

    if (!game::IsSupportedExecutable()) {
        Log("ShowRaster hook skipped: module base is not 0x00400000");
        return;
    }

    InstallSignatureHook(game::kShowRaster, game::kShowRasterSignature,
                         sizeof(game::kShowRasterSignature),
                         reinterpret_cast<void*>(&HookedShowRaster),
                         reinterpret_cast<void**>(&g_originalShowRaster),
                         "ShowRaster");
}

}  // namespace bm
