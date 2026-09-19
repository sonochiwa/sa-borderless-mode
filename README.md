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
executable. The refresh-rate synchronisation and the frame-delay patch use GTA
San Andreas 1.0 US addresses and verify the expected bytes before patching; on
another executable those parts are skipped.

## Features

- Converts exclusive fullscreen D3D9 presentation to borderless windowed mode
  and removes the vsync wait by forcing `D3DPRESENT_INTERVAL_IMMEDIATE`.
- Keeps the game's refresh-rate value synchronised with the desktop mode when
  video settings are applied, and preserves the frame-delay patch across
  device resets.
- Keeps working after Alt-Tab and in-game video setting changes by handling
  `IDirect3DDevice9::Reset`.
- Leaves already-windowed setups alone and only removes the vsync wait.
- Blocks Alt+Enter so the game cannot leave borderless mode, and hides the
  Tab of Alt+Tab from the game so the SA-MP scoreboard does not stick open.
- Declares the process DPI aware, so the game starts on a scaled display from
  a folder Windows has no compatibility history for.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable). The borderless
  conversion itself works on other executables; the address-dependent parts
  are skipped there.
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.

## Installation

1. Extract `BorderlessMode.asi` into the GTA San Andreas directory or its
   `scripts` directory.
2. Start the game.

There is nothing to configure. Remove the file to uninstall.

## Building

Visual Studio 2022 (v143), `Release|Win32`. Open `BorderlessMode.sln` or run:

```powershell
msbuild BorderlessMode.sln /t:Rebuild /p:Configuration=Release /p:Platform=Win32
```

The plugin is written to `build\BorderlessMode.asi`.

## Repository Layout

```text
BorderlessMode.sln
README.md
CHANGELOG.md
LICENSE
.github\workflows\
  build.yml                     Debug and Release build on every push
  release.yml                   Tagged release build, checksum and attestation
src\
  BorderlessMode.cpp            DllMain and startup order
  BorderlessMode.rc             Version resource
  BorderlessMode.vcxproj
  resource.h
  version.h
  core\                         Module paths, hook and patch helpers
  d3d9\                         Present-parameter conversion and the D3D9 device hooks
  game\                         Everything tied to GTA San Andreas 1.0 US addresses
  input\                        Key-state, message-pump and cursor filters
  window\                       Borderless geometry, the window procedure, display-mode guard
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
cannot be undone from inside the process. Untick it.

### Other plugins

`SAMPGraphicRestore.asi` NOPs the instruction in GTA's WM_ACTIVATE handler that
marks the game focused again. This plugin swallows WM_SETFOCUS, GTA's only other
way of setting that flag, so that Alt+Tab does not open the ESC menu every time.
With both installed and nothing setting the flag, the game used to sit in its
unfocused idle loop - one message pump and a 100 ms sleep per iteration, no
rendering and no input - from the first Alt+Tab onwards. Since v1.6.1 the plugin
sets the flag itself, and the two work together.

### Key state

The game and SA:MP poll the global key state (`GetKeyState`,
`GetAsyncKeyState`, `GetKeyboardState`). The plugin mutes those APIs while
another process owns the foreground so held keys from Alt+Tab cannot leak into
the game when it regains focus.

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
