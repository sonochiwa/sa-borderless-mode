# Borderless Mode

`BorderlessMode.asi` is a GTA San Andreas plugin that runs the game in a
borderless fullscreen window without capping the frame rate at the monitor
refresh rate.

The game runs in exclusive fullscreen, which ties its frame rate to the
refresh rate, turns every Alt+Tab into a display mode switch and confuses
overlays and capture tools. The plugin runs it in a borderless window
covering the monitor, without the vsync wait, and keeps Alt+Tab, overlays
and capture tools working.

## Features

- Borderless window covering the monitor, no vsync wait.
- Survives Alt+Tab and in-game video setting changes.
- Blocks Alt+Enter, and hides the Tab of Alt+Tab from the game so the SA-MP
  scoreboard does not stick open.
- Keys held while switching back to the game are not passed on as presses.
- Keeps the last frame on screen while the game sits in the background, so
  overlays such as the NVIDIA App keep tracking it across Alt+Tab.
- Already-windowed setups only lose the vsync wait.
- Declares the process DPI aware, so the game starts on a scaled display.
- FPS counter on Alt+F11 that counts the game's own frames, for when
  external counters cannot measure a borderless window.

## Requirements

- GTA San Andreas 1.0 US (Compact or Hoodlum executable). The borderless
  conversion itself works on other executables; the address-dependent parts
  are skipped there.
- An ASI loader, such as Silent's ASI Loader or Ultimate ASI Loader.

## Installation

1. Extract `BorderlessMode.asi` and `BorderlessMode.ini` into the GTA San
   Andreas directory or its `scripts` directory.
2. Start the game.

Remove the files to uninstall.

If **Override high DPI scaling behavior** is ticked in the compatibility
properties of `gta_sa.exe`, untick it; it overrides the plugin and the game
shuts down at startup above 100% scaling.

## Configuration

`BorderlessMode.ini` next to the plugin, created with these defaults when it
is missing:

| Setting | Default | Meaning |
| --- | ---: | --- |
| `[fpsCounter]` | | |
| `show` | `0` | `1` shows the FPS counter. Written back whenever the counter is toggled in game. |
| `hotkeyEnabled` | `1` | `0` disables the hotkey without removing the key. |
| `hotkeyModifier` | `18` | Modifier as a decimal Win32 virtual-key code. `18` is Alt; `0` means no modifier. |
| `hotkeyKey` | `122` | Main key as a decimal Win32 virtual-key code; `122` is F11. `0` disables the hotkey. |

The borderless conversion itself has no settings. The hotkey is read once at
startup.

## Release Integrity

Releases are built by GitHub Actions from the tagged commit and carry a
SHA-256 file and a build-provenance attestation:

```text
gh attestation verify BorderlessMode-vX.Y.Z.zip -R sonochiwa/sa-borderless-mode
```

## License

MIT. See [LICENSE](LICENSE).
