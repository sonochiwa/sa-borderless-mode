# Changelog

## v1.4.1 - 2026-07-29

- Added a GitHub Actions release workflow that builds release binaries from
  tagged commits.
- Added SHA-256 checksum files and signed GitHub artifact attestations so
  downloaded release archives can be verified against their source workflow.

## v1.4.0 - 2026-07-29

- Removed Anti-AFK and its setting from the INI. This feature is better suited
  to a separate plugin.
- Fixed Alt+Tab and Win+D behavior. GTA now stays minimized when another
  program is opened instead of appearing behind it.
- Fixed cases where GTA could raise its window or take focus while another
  program was being used.
- Borderless mode is now reapplied only after GTA is actually restored.
- Improved the log to make window and focus problems easier to diagnose.

## v1.3.0 - 2026-07-27

- Added a built-in FPS counter, shown with F11 by default. Its state and hotkey
  can be changed in the INI.
- Fixed Alt+Enter switching SA:MP out of borderless mode.
- Built in the refresh-rate and frame-delay fixes, including support for
  changing video settings without losing them.
- Restored support for 16-bit video modes.
- Reorganized the INI while keeping old configurations compatible.

## v1.2.0

- Fixed the SA:MP scoreboard opening or getting stuck after Alt+Tab.
- Prevented keys pressed in another program from leaking into GTA when the
  game regains focus.
- Made Alt+Tab faster and smoother by keeping the desktop display mode intact.

## v1.1.0

- Improved compatibility with other mods, overlays, and graphics wrappers.
- Reduced unnecessary graphics resets.
- Added a diagnostic log for graphics and window problems.

## v1.0.0

- First release.
- Added borderless fullscreen without locking FPS to the monitor refresh rate.
- Borderless mode survives Alt+Tab and in-game video setting changes.
- Added automatic creation of the default INI.
