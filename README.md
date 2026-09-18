# Borderless Mode

`BorderlessMode.asi` is a standalone GTA San Andreas plugin that runs the game
in a borderless fullscreen window without capping the frame rate at the
monitor refresh rate.

The game creates an exclusive fullscreen Direct3D 9 device. That ties its
frame rate to the refresh rate, turns every Alt-Tab into a display mode
switch, and confuses overlays and capture tools. The plugin converts the
device's present parameters to a windowed swap chain with immediate
presentation, restyles the game window into a borderless one covering the
monitor, and then keeps the game from noticing: display-mode changes, cursor
confinement, focus-driven key state and the message pump are filtered so the
game behaves as it would in exclusive mode.

The borderless conversion and the Direct3D hooks do not depend on a specific
executable. The built-in FPS counter, the refresh-rate synchronisation and the
frame-delay patch use GTA San Andreas 1.0 US addresses and verify the expected
bytes before patching; on another executable those parts are skipped.

## Features

- Converts exclusive fullscreen D3D9 presentation to borderless windowed mode
  and removes the vsync wait by forcing `D3DPRESENT_INTERVAL_IMMEDIATE`.
- Keeps the game's refresh-rate value synchronised with the desktop mode when
  video settings are applied, and preserves the frame-delay patch across
  device resets.
- Keeps working after Alt-Tab and in-game video setting changes by handling
  `IDirect3DDevice9::Reset`.
- Leaves already-windowed setups alone and only removes the vsync wait.
- Shows the game's actual in-game FPS on Alt+F11, because external counters
  may report the window's refresh rate instead.
- Blocks Alt+Enter so the game cannot leave borderless mode, and hides the
  Tab of Alt+Tab from the game so the SA-MP scoreboard does not stick open.
- Declares the process DPI aware, so the game starts on a scaled display from
  a folder Windows has no compatibility history for.
- Creates the default INI when it is missing.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable). The borderless
  conversion itself works on other executables; the address-dependent parts
  are skipped there.
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.

## Installation

1. Extract `BorderlessMode.asi` and `BorderlessMode.ini` into the GTA San
   Andreas directory or its `scripts` directory.
2. Start the game.

## Configuration

```ini
# Borderless Mode v1.7.1
# Created by sonochiwa
# Source code: https://github.com/sonochiwa/sa-borderless-mode
# Default FPS counter hotkey: Alt + F11

[general]
log=0

[fpsCounter]
show=0
hotkeyEnabled=1
hotkeyModifier=18
hotkeyKey=122
```

| Setting | Default | Meaning |
| --- | ---: | --- |
| `[general]` | | |
| `log` | `0` | `1` writes `BorderlessMode.log` next to the plugin, recreated on every start. |
| `[fpsCounter]` | | |
| `show` | `0` | Shows the game's actual in-game FPS. Written back whenever the counter is toggled in game. |
| `hotkeyEnabled` | `1` | Enables the hotkey. `0` disables it without removing the key. |
| `hotkeyModifier` | `18` | Modifier as a decimal Win32 virtual-key code. `18` is Alt; `0` means no modifier. |
| `hotkeyKey` | `122` | Main key as a decimal Win32 virtual-key code; `122` is F11. Removing the key or setting it to `0` disables the hotkey. |

The FPS counter is built in because external overlays may count how often the
game window is refreshed rather than how many frames the game produces. The
hotkey toggles the counter and saves the new `show` value; it does not reload
the INI. The main key is intercepted only while the configured modifier is
held. Settings are read once at startup.

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

### Other plugins

`SAMPGraphicRestore.asi` NOPs the instruction in GTA's WM_ACTIVATE handler that
marks the game focused again. This plugin swallows WM_SETFOCUS, GTA's only other
way of setting that flag, so that Alt+Tab does not open the ESC menu every time.
With both installed and nothing setting the flag, the game used to sit in its
unfocused idle loop - one message pump and a 100 ms sleep per iteration, no
rendering and no input - from the first Alt+Tab onwards. Since v1.6.1 the plugin
sets the flag itself, and the two work together.

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

Legacy `[BorderlessMode]` and `[SABorderless]` sections are still read.

The game and SA:MP poll the global key state (`GetKeyState`,
`GetAsyncKeyState`, `GetKeyboardState`). The plugin mutes those APIs while
another process owns the foreground so held keys from Alt+Tab cannot leak into
the game when it regains focus.

## Building

Visual Studio 2022 (v143), `Release|Win32`. Open `BorderlessMode.sln` or run:

```powershell
msbuild BorderlessMode.sln /t:Rebuild /p:Configuration=Release /p:Platform=Win32
```

The plugin is written to `build\BorderlessMode.asi` next to a copy of the
INI.

## Repository Layout

```text
BorderlessMode.sln
README.md
CHANGELOG.md
LICENSE
.github\workflows\
  build.yml                     Debug and Release build on every push
  release.yml                   Tagged release build, checksum and attestation
Config\
  BorderlessMode.ini            Canonical configuration, embedded as RCDATA
src\
  BorderlessMode.cpp            DllMain and startup order
  BorderlessMode.rc             Version resource and the embedded INI
  BorderlessMode.vcxproj
  resource.h
  version.h
  core\                         Logging, configuration, module paths, hook and patch helpers
  d3d9\                         Present-parameter conversion and the D3D9 device hooks
  game\                         Everything tied to GTA San Andreas 1.0 US addresses
  input\                        Key-state, message-pump and cursor filters
  window\                       Borderless geometry, the window procedure, display-mode guard
tools\
  repro.ps1                     Reproduction harness for startup bugs
vendor\
  minhook\                      MinHook, compiled into the plugin
```

Every hard-coded game address lives in `src\game\addresses.h`; nothing
outside `src\game\` depends on a specific executable.

## How It Works

On load, the plugin hooks the `Direct3DCreate9` export from the `d3d9.dll`
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

## Release Integrity

Tagged releases are built by GitHub Actions from the tagged commit. Each
release carries `BorderlessMode-vX.Y.Z.zip`, its SHA-256 in
`BorderlessMode-vX.Y.Z.zip.sha256` and a signed build-provenance attestation,
which proves that the archive was produced by this repository's workflow
from that revision. It does not prove the code is bug-free.

```text
gh attestation verify BorderlessMode-vX.Y.Z.zip -R sonochiwa/sa-borderless-mode
```

## License

MIT. See [LICENSE](LICENSE).
