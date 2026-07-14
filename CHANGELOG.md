# Changelog

## v1.2.0

- Suppressed the TAB input that can leak through Alt+Tab and open the SA:MP
  scoreboard after returning to the game.
- Added short refocus input muting so delayed or queued TAB state from Alt+Tab
  is treated as released when the game regains focus.
- Blocked background keyboard input from leaking into the game while another
  process owns the foreground window, especially when `AntiAFK=1` keeps the
  game running in the background.
- Kept the desktop display mode intact while borderless mode is active, which
  avoids slow Alt+Tab and display-mode churn caused by Windows AppCompat
  fullscreen shims.

## v1.1.0

- Stopped mutating the game's original D3D present-parameter struct directly.
  The plugin now passes converted copies to D3D, reducing conflicts with other
  mods, overlays, and wrappers that inspect or reuse the original parameters.
- Improved redundant D3D reset handling so repeated identical reset requests no
  longer cause unnecessary mode work.
- Added diagnostics for D3D creation, reset handling, present-parameter
  conversion, and window/display state changes.

## v1.0.0

- Initial ASI plugin for GTA San Andreas borderless fullscreen mode.
- Converted exclusive fullscreen D3D9 presentation to borderless windowed mode.
- Forced `D3DPRESENT_INTERVAL_IMMEDIATE` to avoid the fullscreen frame-delay /
  refresh-rate wait.
- Hooked `IDirect3DDevice9::Reset` so borderless mode survives Alt+Tab and
  in-game video setting changes.
- Added optional `AntiAFK=1` mode to keep the game active while minimized or in
  the background.
- Added default `BorderlessMode.ini` generation next to the ASI.
- Vendored MinHook sources and Visual Studio 2022 project files.
