# Changelog

## 1.9.0

- Fixed the NVIDIA App overlay losing the game after Alt+Tab, which left its
  FPS counter frozen or missing.
- Restored the FPS counter; type `FPSCOUNTER` in game to show or hide it,
  `BorderlessMode.ini` holds its settings.

## 1.8.1

- Added `README.txt` to the release archive.

## 1.8.0

- Removed the INI, the log, the debug switches and the FPS counter; the
  plugin has nothing to configure.

## 1.7.1

- Changed the INI to UTF-8, created from the file compiled into the plugin.
- Added version information to the plugin file.
- Removed `README.txt` from the release archive.

## 1.7.0

- Added the MIT license.
- Removed `.clang-format` and `.editorconfig`.

## 1.6.1

- Fixed the game freezing on the first Alt+Tab with
  `SAMPGraphicRestore.asi` installed.

## 1.6.0

- Changed the FPS counter key to Alt+F11.
- Fixed white rectangles flashing in the corner at startup.
- Fixed the game closing itself at startup on a display scaled above 100%,
  and the window not covering the whole screen there.
- Added an optional `[debug]` section.
- Fixed a hitch when toggling the FPS counter and slow startup with logging
  on.

## 1.5.0

- Fixed the game closing by itself right after launch.
- Fixed a crash when quitting.
- Made startup more dependable.

## 1.4.2

- Fixed NVIDIA Overlay disappearing or opening with only the Windows
  cursor visible.
- Removed unnecessary graphics resets when the game regains focus.
- Changed the FPS counter to the game's own frame count and moved it above
  the HUD.

## 1.4.1

- Added the GitHub Actions release workflow with checksums and attestation.

## 1.4.0

- Removed Anti-AFK.
- Fixed Alt+Tab and Win+D: the game stays minimized and no longer steals
  focus.
- Improved the log.

## 1.3.0

- Added a built-in FPS counter on F11.
- Fixed Alt+Enter leaving borderless mode.
- Built in the refresh-rate and frame-delay fixes.
- Restored 16-bit video modes.

## 1.2.0

- Fixed the SA-MP scoreboard opening or sticking after Alt+Tab.
- Fixed keys pressed in another program leaking into the game.
- Made Alt+Tab faster by keeping the desktop display mode.

## 1.1.0

- Improved compatibility with other mods, overlays and graphics wrappers.
- Reduced unnecessary graphics resets.
- Added a diagnostic log.

## 1.0.0

- Borderless fullscreen without locking FPS to the refresh rate.
- Survives Alt+Tab and in-game video setting changes.
- INI created when missing.
