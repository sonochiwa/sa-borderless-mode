# Changelog

## v1.3.0 - 2026-07-27

- Added an in-game real presentation-rate counter, toggled with a configurable
  key (`F11` by default). The enabled state is now saved to the INI.
- Added optional modifier-plus-key FPS hotkeys and an explicit hotkey enable
  switch. Removing `hotkeyKey` or setting it to `0` now disables key handling.
- Blocked SA:MP's `Alt+Enter` window-mode switch at the message-queue level so
  the game stays in borderless mode without intercepting normal Enter input.
- Integrated RefreshRateFix behavior: the desktop refresh rate is written to
  GTA whenever it applies a video mode.
- Integrated and persistently reapplied the 14 ms NoFrameDelay patch after
  video-mode changes and D3D device resets.
- Restored 16-bit video-mode selection, with a 32-bit compatibility retry for
  drivers that reject a 16-bit windowed backbuffer.
- Split the default INI into lowercase `[general]` and `[fpsCounter]` sections
  while retaining compatibility with existing `[BorderlessMode]` and
  `[SABorderless]` configurations.
- Removed the experimental window-resizing, frame-scaling, aspect-ratio and
  multi-layer Alt+Enter workarounds after identifying the minimal SA:MP
  message responsible for the mode switch.

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
