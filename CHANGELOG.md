# Changelog

## 1.7.1

- Changed the INI to UTF-8; it is now created from the canonical file
  compiled into the plugin, byte for byte. An existing UTF-16 file keeps
  working.
- Added version information to the plugin file.
- Removed `README.txt` from the release archive; the repository README is the
  documentation.

## 1.7.0

- Added the MIT license. The repository shipped ten releases without one, so
  the terms were nowhere in the source tree.
- Removed `.clang-format` and `.editorconfig`. They are local tooling
  preferences rather than part of what the plugin ships or how it builds.

## 1.6.1

- Fixed the game freezing for good on the first Alt+Tab when
  `SAMPGraphicRestore.asi` is installed alongside this plugin. GTA keeps a
  "window has focus" flag and, while it is clear, WinMain does nothing but
  pump messages and sleep 100 ms per iteration - no rendering, no input, no
  game logic. Stock GTA sets it again from two places, its WM_ACTIVATE and its
  WM_SETFOCUS handler. SAMPGraphicRestore overwrites the WM_ACTIVATE one with
  NOPs, and this plugin swallows WM_SETFOCUS to keep the ESC menu from opening
  on every Alt+Tab back, so with both installed nothing set the flag again and
  the game sat there frozen. The plugin now sets it itself when the window
  takes focus, which is what GTA would have done in either handler.

## 1.6.0

- The FPS counter is now toggled with **Alt+F11** instead of F11, so a bare
  F11 stays free for overlays and recording software. Existing configurations
  keep the key they already have; to move to the new default, set
  `hotkeyModifier=18` or delete `BorderlessMode.ini` and let it be recreated.
- Fixed white rectangles flashing in the top-left corner of the screen about a
  second into startup. The plugin has to show the game's window itself, and it
  was making the window visible before moving it: for a few frames the game's
  unpainted 640x480 startup window appeared in the corner, showing whatever
  happened to be in its surface. The window is now revealed only once it
  already covers the monitor, and its background is painted black so an
  unpainted frame blends into the loading screen.
- Fixed the game closing itself a second after startup on a display with
  scaling above 100%. This is the failure that looked random and unfixable: a
  freshly assembled modpack in a new folder would not start, while the same
  files in a folder that had been launched a few times were fine, and starting
  the game once without the plugin appeared to repair it for good. The plugin
  now tells Windows that the game understands display scaling, instead of
  waiting for Windows to work it out on its own.
- The borderless window now covers the whole screen on a scaled display. It
  used to be sized as if the monitor were smaller - 2048x1152 on a 2560x1440
  screen at 125%.
- Added an optional `[debug]` section for turning parts of the plugin off when
  tracking down a conflict.
- Toggling the FPS counter no longer writes its setting to disk on the frame
  you press the key, which could cause a brief hitch.
- Diagnostic logging is much cheaper. It used to force a disk write per line,
  which slowed the game's startup enough to change the behaviour being
  diagnosed. Logs are still complete after a crash.
- A 16-bit video mode being switched to 32-bit, which borderless mode has
  always had to do, now says so in the log instead of silently changing how
  the game looks.
- Internal only, no behaviour changes: the single 2,500-line source file was
  split into modules (core, d3d9, window, input, game), every hard-coded game
  address moved into one header, and an unreachable leftover of the old
  Alt+Tab "TAB re-injection" experiment was removed.

## 1.5.0

- Fixed the game closing by itself right after launch. On a newly assembled
  build the game could flash on screen for an instant and disappear, and
  whether it started at all felt random: turning borderless mode off, starting
  the game once, and turning it back on would often work around it for a
  while. This no longer happens.
- Fixed a crash when quitting. Closing the game could end with an error and a
  Windows crash report instead of a clean exit.
- Made starting up more dependable in general. Part of the plugin's setup used
  to run while the game was still loading, where it could get in the way of
  the game itself and of the graphics driver. It now waits for a safe moment.

## 1.4.2

- Fixed NVIDIA Overlay disappearing or opening with only the Windows cursor
  visible, including after Alt+Tab and restoring the game.
- Removed unnecessary graphics resets when GTA regains focus. This keeps
  third-party overlays from losing their rendering layer.
- Changed the built-in FPS counter to use GTA's internal frame count instead
  of the window presentation rate. External counters may still show the
  monitor refresh rate in borderless mode.
- Moved the FPS counter above GTA and SA:MP interface elements.

## 1.4.1

- Added a GitHub Actions release workflow that builds release binaries from
  tagged commits.
- Added SHA-256 checksum files and signed GitHub artifact attestations so
  downloaded release archives can be verified against their source workflow.

## 1.4.0

- Removed Anti-AFK and its setting from the INI. This feature is better suited
  to a separate plugin.
- Fixed Alt+Tab and Win+D behavior. GTA now stays minimized when another
  program is opened instead of appearing behind it.
- Fixed cases where GTA could raise its window or take focus while another
  program was being used.
- Borderless mode is now reapplied only after GTA is actually restored.
- Improved the log to make window and focus problems easier to diagnose.

## 1.3.0

- Added a built-in FPS counter, shown with F11 by default. Its state and hotkey
  can be changed in the INI.
- Fixed Alt+Enter switching SA:MP out of borderless mode.
- Built in the refresh-rate and frame-delay fixes, including support for
  changing video settings without losing them.
- Restored support for 16-bit video modes.
- Reorganized the INI while keeping old configurations compatible.

## 1.2.0

- Fixed the SA:MP scoreboard opening or getting stuck after Alt+Tab.
- Prevented keys pressed in another program from leaking into GTA when the
  game regains focus.
- Made Alt+Tab faster and smoother by keeping the desktop display mode intact.

## 1.1.0

- Improved compatibility with other mods, overlays, and graphics wrappers.
- Reduced unnecessary graphics resets.
- Added a diagnostic log for graphics and window problems.

## 1.0.0

- First release.
- Added borderless fullscreen without locking FPS to the monitor refresh rate.
- Borderless mode survives Alt+Tab and in-game video setting changes.
- Added automatic creation of the default INI.
