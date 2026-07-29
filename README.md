# BorderlessMode.asi

Borderless fullscreen windowed mode for **GTA San Andreas** without capping FPS
at the monitor refresh rate.

BorderlessMode is a standalone ASI plugin. It uses WinAPI, D3D9 headers from the
Windows SDK, and MinHook sources vendored in `vendor\minhook`.

## What It Does

- Converts exclusive fullscreen D3D9 presentation to borderless windowed mode.
- Removes the vsync wait by forcing `D3DPRESENT_INTERVAL_IMMEDIATE`.
- Keeps GTA's refresh-rate value synchronized with the current desktop mode
  when video settings are applied.
- Preserves the NoFrameDelay patch across D3D device resets and supports
  selecting 16-bit video modes.
- Keeps working after alt-tab and in-game video setting changes by also handling
  `IDirect3DDevice9::Reset`.
- Leaves already-windowed setups alone and only removes the vsync wait.
- Press **F11** to show or hide the real D3D presentation rate in-game.
- Blocks **Alt+Enter** so the game cannot accidentally leave borderless mode.
- Hides the TAB press of Alt+Tab from the game, so the SA:MP scoreboard no
  longer gets stuck open after switching back.

The core borderless D3D9 hooks do not depend on a specific GTA executable.
The integrated RefreshRateFix and NoFrameDelay features use GTA SA 1.0 US
addresses and verify the expected code signatures before patching. On an
unknown executable, those address-dependent patches are skipped safely.

## Installation

Copy these files from the release archive to your GTA SA folder, or to the folder
used by your ASI loader:

```text
BorderlessMode.asi
BorderlessMode.ini
```

Ultimate ASI Loader and CLEO ASI loading are both fine.

If `BorderlessMode.ini` is missing, the plugin creates it next to
`BorderlessMode.asi` with default values.

## Configuration

Edit `BorderlessMode.ini` and restart the game.

```ini
# BorderlessMode v1.4.1
# Created by sonochiwa
# Source code: https://github.com/sonochiwa/sa-borderless-mode
# Default FPS toggle hotkey: F11

[general]
log=0

# Shows GTA's actual in-game FPS. External tools such as NVIDIA
# counters may show the window's refresh rate instead.
[fpsCounter]
show=0
hotkeyEnabled=1
hotkeyModifier=0
hotkeyKey=122
```

| Section | Key | Default | Meaning |
| ------- | --- | ------- | ------- |
| `general` | `log` | `0` | `1` writes `BorderlessMode.log` next to the ASI for diagnostics. |
| `fpsCounter` | `show` | `0` | Shows the real D3D presentation rate. Its value is saved whenever the counter is toggled in game. |
| `fpsCounter` | `hotkeyEnabled` | `1` | Enables hotkey handling. Set to `0` to disable it without removing the key. |
| `fpsCounter` | `hotkeyModifier` | `0` | Optional modifier as a decimal Win32 virtual-key code. `0` means no modifier. |
| `fpsCounter` | `hotkeyKey` | `122` | Main key as a decimal Win32 virtual-key code (`122` is F11). Removing the key or setting it to `0` disables hotkey handling. |

> **Why the FPS counter is built in:** External programs, including NVIDIA
> tools and other FPS overlays, may show a misleading value with this
> borderless mode. They can count how often the game window is refreshed
> instead of how many frames GTA is actually producing. The built-in counter
> shows GTA's real in-game FPS, so it is the value to use when checking
> performance.

For example, use `hotkeyModifier=18` and `hotkeyKey=89` for **Alt + Y**.
The main key is intercepted only while the configured modifier is held.

The log is recreated on each game start. If the game hangs or shows a black
screen, close the process and send `BorderlessMode.log` from the GTA SA folder.

Legacy `[BorderlessMode]` and `[SABorderless]` configurations remain supported.

The game and SA:MP poll the global key state (`GetKeyState`,
`GetAsyncKeyState`, `GetKeyboardState`). The plugin mutes those APIs while
another process owns the foreground so held keys from Alt+Tab cannot leak into
the game when it regains focus.

The default release config is stored in `Config\BorderlessMode.ini`.

## Building

Open `BorderlessMode.sln` in Visual Studio 2022 and build `Release|Win32`.

Command-line build:

```bat
msbuild BorderlessMode.sln /p:Configuration=Release /p:Platform=Win32
```

The ASI is written to:

```text
build\BorderlessMode.asi
```

The local release archive is:

```text
build\BorderlessMode-v1.4.1.zip
```

`build\` is generated output and is intentionally ignored by git.

## Release Integrity

Tagged releases are compiled and packaged by GitHub Actions. Each release
contains the ZIP archive, a SHA-256 checksum file, and a signed GitHub artifact
attestation that binds the archive to its source commit and workflow:

```bat
gh attestation verify BorderlessMode-v1.4.1.zip -R sonochiwa/sa-borderless-mode
```

## Repository Layout

```text
Config\BorderlessMode.ini        Default release config
src\dllmain.cpp                  ASI source
src\BorderlessMode.vcxproj       Visual C++ project
vendor\minhook\                  Vendored MinHook sources
BorderlessMode.sln               Visual Studio solution
```

## How It Works

On load, BorderlessMode hooks the `Direct3DCreate9` export from the `d3d9.dll`
used by the game. Once the game creates its `IDirect3D9` object, the plugin hooks
the `IDirect3D9::CreateDevice` code found through that object's vtable.

When the game requests exclusive fullscreen, the present parameters are changed
to windowed mode with immediate presentation. The game window is then stripped of
its borders and stretched to the monitor it is on. If device creation fails with
the modified parameters, the plugin restores the original parameters and retries.

After the device exists, `IDirect3DDevice9::Reset` is hooked as well, so the mode
survives alt-tab and video setting changes. Hook-side changes are wrapped in SEH
so unexpected wrapper behavior falls back to the original game call instead of
crashing the game.
