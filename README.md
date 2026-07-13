# BorderlessMode.asi

Borderless fullscreen windowed mode for **GTA San Andreas** without capping FPS
at the monitor refresh rate.

BorderlessMode is a standalone ASI plugin. It uses WinAPI, D3D9 headers from the
Windows SDK, and MinHook sources vendored in `vendor\minhook`.

## What It Does

- Converts exclusive fullscreen D3D9 presentation to borderless windowed mode.
- Removes the vsync wait by forcing `D3DPRESENT_INTERVAL_IMMEDIATE`.
- Keeps working after alt-tab and in-game video setting changes by also handling
  `IDirect3DDevice9::Reset`.
- Leaves already-windowed setups alone and only removes the vsync wait.
- Optionally keeps the game active in the background with `AntiAFK=1`.
- Hides the TAB press of Alt+Tab from the game, so the SA:MP scoreboard no
  longer gets stuck open after switching back.

No GTA SA executable addresses are used. The plugin hooks D3D9, so it is meant
to work with GTA SA 1.0, 1.01, Steam builds, SA:MP, overlays and D3D9 wrappers.

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
[BorderlessMode]
Log=0
AntiAFK=0
```

| Key | Default | Meaning |
| --- | ------- | ------- |
| `Log` | `0` | `1` writes `BorderlessMode.log` next to the ASI for diagnostics. |
| `AntiAFK` | `0` | `1` makes the game keep running while minimized or in the background. |

The log is recreated on each game start. If the game hangs or shows a black
screen, close the process and send `BorderlessMode.log` from the GTA SA folder.

Anti-AFK rewrites focus-loss window messages such as `WM_ACTIVATE`,
`WM_ACTIVATEAPP`, `WM_NCACTIVATE` and `WM_KILLFOCUS`. Because the game and
SA:MP poll the global key state (`GetKeyState`, `GetAsyncKeyState`,
`GetKeyboardState`) every frame, the plugin mutes those APIs whenever the
foreground window belongs to another process, so typing in other windows no
longer leaks into the game while it runs in the background.

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
build\BorderlessMode-v1.1.0.zip
```

`build\` is generated output and is intentionally ignored by git.

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
