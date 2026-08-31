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
- Press **Alt+F11** to show or hide GTA's actual in-game FPS.
- Blocks **Alt+Enter** so the game cannot accidentally leave borderless mode.
- Hides the TAB press of Alt+Tab from the game, so the SA:MP scoreboard no
  longer gets stuck open after switching back.

The core borderless D3D9 hooks do not depend on a specific GTA executable.
The integrated FPS counter, RefreshRateFix, and NoFrameDelay features use GTA
SA 1.0 US addresses and verify the expected code signatures before patching.
On an unknown executable, those address-dependent features are skipped safely.

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
# BorderlessMode v1.5.0
# Created by sonochiwa
# Source code: https://github.com/sonochiwa/sa-borderless-mode
# Default FPS toggle hotkey: Alt + F11

[general]
log=0

# Shows GTA's actual in-game FPS. External tools such as NVIDIA
# counters may show the window's refresh rate instead.
# The hotkey is given as decimal Win32 virtual-key codes: 18 is Alt,
# 122 is F11. Set hotkeyModifier=0 for a bare key with no modifier.
[fpsCounter]
show=0
hotkeyEnabled=1
hotkeyModifier=18
hotkeyKey=122
```

| Section | Key | Default | Meaning |
| ------- | --- | ------- | ------- |
| `general` | `log` | `0` | `1` writes `BorderlessMode.log` next to the ASI for diagnostics. |
| `fpsCounter` | `show` | `0` | Shows GTA's actual in-game FPS. Its value is saved whenever the counter is toggled in game. |
| `fpsCounter` | `hotkeyEnabled` | `1` | Enables hotkey handling. Set to `0` to disable it without removing the key. |
| `fpsCounter` | `hotkeyModifier` | `18` | Modifier as a decimal Win32 virtual-key code. `18` is Alt; `0` means no modifier. |
| `fpsCounter` | `hotkeyKey` | `122` | Main key as a decimal Win32 virtual-key code (`122` is F11). Removing the key or setting it to `0` disables hotkey handling. |

> **Why the FPS counter is built in:** External programs, including NVIDIA
> tools and other FPS overlays, may show a misleading value with this
> borderless mode. They can count how often the game window is refreshed
> instead of how many frames GTA is actually producing. The built-in counter
> shows GTA's real in-game FPS, so it is the value to use when checking
> performance.

For example, use `hotkeyModifier=18` and `hotkeyKey=89` for **Alt + Y**, or
`hotkeyModifier=0` and `hotkeyKey=122` for a bare **F11**.
The main key is intercepted only while the configured modifier is held.

The log is recreated on each game start. If the game hangs or shows a black
screen, close the process and send `BorderlessMode.log` from the GTA SA folder.

### Display scaling

GTA SA never tells Windows it understands display scaling. Above 100% scaling
Windows therefore hands it a virtual desktop: on a 2560x1440 monitor at 125%
the game believes the screen is 2048x1152. Exclusive fullscreen hides this, but
a borderless window whose back buffer is the real monitor size on a virtual
desktop makes the game shut itself down a second after startup.

Windows works around this on its own, eventually, by recording a `HIGHDPIAWARE`
compatibility layer for that particular `gta_sa.exe` path. That is why a freshly
assembled modpack in a new folder could fail while the same files in a folder
that had been launched a few times were fine, and why launching once without the
plugin appeared to repair it. The plugin now declares the awareness itself on
every launch, so none of that is left to chance.

There is deliberately no setting for it. At 100% scaling Windows virtualizes
nothing and declaring awareness changes nothing; above 100% the game does not
start without it. The in-game resolution makes no difference either way: the
window is sized from the monitor and the back buffer keeps whatever the game
selected.

One setting still overrides the plugin. If **Properties -> Compatibility ->
Change high DPI settings -> Override high DPI scaling behavior** is ticked on
`gta_sa.exe`, the shim engine applies that before any plugin code runs and it
cannot be undone from inside the process. Untick it. With `log=1` this shows up
as `process DPI awareness: set=1 aware=0` followed by a `STILL UNAWARE` line.

### Diagnostics

An optional `[debug]` section turns parts of the plugin off, to find what a
troublesome setup is tripping over. All of them default to `0`:

```ini
[debug]
disableWindowHook=0
disableInputFilters=0
disableMessagePump=0
disableCursorGuard=0
disableDisplayGuard=0
disableGamePatches=0
disableBorderlessStyle=0
disableConversion=0
disableDpiAware=0
heartbeat=0
```

`disableConversion=1` leaves the plugin loaded but doing nothing, which is the
useful control when deciding whether a problem is the plugin at all. The active
combination is written to the log as a `debug switches:` line. `heartbeat=1`
adds a line every 500 ms for a minute, which tells a process that died apart
from one whose window messages merely stopped arriving.

Do not disable `borderlessStyle` while leaving `conversion` on: that gives the
device a windowed swap chain while the window keeps its fullscreen geometry,
which does not work by itself and will look like the conversion failing.

`tools\repro.ps1` drives all of this. Its `-Fresh` switch is the important
part: it points a directory junction at the modpack, so the game runs from a
path Windows has no compatibility history for, and clears that history again
before every launch. That is what "a brand new modpack folder" means in
practice, and it is the difference between a bug that reproduces on demand and
one that looks random. Nothing is copied.

```powershell
# Does a fresh install of this modpack start at all?
.\tools\repro.ps1 -Root 'D:\modpacks\mypack' -Fresh

# It does not - which part of the plugin is it?
.\tools\repro.ps1 -Root 'D:\modpacks\mypack' -Fresh -Matrix

# Does a compatibility shim explain it?
.\tools\repro.ps1 -Root 'D:\modpacks\mypack' -Fresh -CompatLayer DPIUNAWARE
```

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
build\BorderlessMode-v1.5.0.zip
```

`build\` is generated output and is intentionally ignored by git.

## Release Integrity

Tagged releases are compiled and packaged by GitHub Actions. Each release
contains the ZIP archive, a SHA-256 checksum file, and a signed GitHub artifact
attestation that binds the archive to its source commit and workflow:

```bat
gh attestation verify BorderlessMode-v1.5.0.zip -R sonochiwa/sa-borderless-mode
```

## Repository Layout

```text
Config\BorderlessMode.ini        Default release config
src\dllmain.cpp                  DllMain and startup order
src\version.h                    Version string used by the default config
src\core\                        Logging, config, module paths, hook helpers
src\d3d9\                        Present-parameter conversion, D3D9 hooks
src\window\                      Borderless geometry, window proc, display mode
src\input\                       Key-state, message-pump and cursor filters
src\game\                        Everything tied to GTA SA 1.0 US addresses
src\BorderlessMode.vcxproj       Visual C++ project
tools\repro.ps1                  Reproduction harness for startup bugs
vendor\minhook\                  Vendored MinHook sources
BorderlessMode.sln               Visual Studio solution
```

Every hard-coded GTA address lives in `src\game\addresses.h`; nothing outside
`src\game\` depends on a specific executable.

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
