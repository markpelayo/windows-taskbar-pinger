# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Changed

- **The widget now anchors to the notification area** instead of the taskbar's left edge. At the
  far left it overlapped the Windows 11 weather and news widget; the app buttons between them are
  centred and move as windows open and close, so the notification area is the only stable anchor.
  Its position is re-checked as the tray changes width.
- **Latency text follows the shell's theme.** It was drawn with `GetSysColor(COLOR_BTNTEXT)`, which
  reports the *apps* theme — black under a default Windows 11 install, and unreadable on the dark
  taskbar. It now reads `SystemUsesLightTheme`, which is what the taskbar itself follows.
- **Latency text is 12 pt**, up from 9. The taskbar clock's size works because you already know
  roughly what a clock says; a latency figure has to actually be read.
- **Default grid is 4 rows**, up from 3.
- The "about" menu item points at this repository rather than the macOS one.

### Known issues

- The widget is not draggable; its position is computed rather than chosen.
- Only lightly tested beyond Windows 11 at 100% scaling with a bottom-docked taskbar.

## [1.0.0] — unreleased

First Windows release. A port of
[macos-menubar-pinger](https://github.com/markpelayo/macos-menubar-pinger) 1.1.0, which was itself
a macOS port of the idea behind the old
[Windows taskbar ping widget](https://superuser.com/questions/661132/show-current-ping-to-website-on-taksbar).

### Added

- Live grid of ping results in the Windows taskbar, one cell per packet, filling bottom-left upward
  and rolling over once full.
- Average round trip printed beside the grid, toggleable per monitor.
- Up to 8 independent grids side by side, each with its own host and settings.
- Named profiles: save, load, overwrite and delete a monitor's whole configuration.
- Full configuration from the right-click menu — host, ping frequency (0.25 s to 1 hour), both
  colors with 10 presets plus the system color picker, and the grid's rows, columns, cell size and
  spacing.
- Notification-area icon, so there is always a way to reach the menu.
- Settings persisted to `%APPDATA%\Pinger\settings.ini`.
- GitHub Actions workflow that builds on Windows and attaches a binary to tagged releases.

### Changed from the macOS version

These are deliberate differences, not omissions:

- **ICMP instead of a child process.** macOS parsed the output of `ping -i <interval> -O`. Windows
  uses `IcmpSendEcho` from `iphlpapi`, sending echo requests from inside the process. No
  `fork`/`exec`, no output parsing, no localised console text to scrape, and no administrator
  rights — unlike a hand-rolled raw ICMP socket.
- **No `-O` watchdog.** It existed to cover `ping` builds that did not report timeouts inline.
  `IcmpSendEcho` always returns or times out, so the cadence is ours to keep and the watchdog is
  unnecessary.
- **Taskbar embedding replaces `NSStatusItem`.** Windows has no arbitrary-width status slot; see
  the README for how this works and what it costs.
- **Profile deletion is a submenu**, not an ✕ button on each row. Win32 menu items cannot host
  child controls the way the macOS `ProfileRowView` did.
- **Settings are INI, not a plist.** Hand-editable, and a correct parser is eighty lines rather
  than several hundred.
- **`Cell size` and `Cell spacing` are in pixels**, not points, and scale with DPI.

### Not ported

- **IPv6.** `Icmp6SendEcho2` needs a bound source address and a different reply layout. An
  IPv6-only target reports as unreachable rather than silently doing nothing.
- **Per-grid hover tooltips.** The notification-area icon shows the first monitor's stats; a
  tooltip per grid would need a tracking control for each.
- **Keyboard shortcuts.** The macOS menu had ⌘L, ⌘D, ⌘S and friends. Context menus opened by
  right-click have no equivalent accelerator context.

[Unreleased]: https://github.com/markpelayo/windows-taskbar-pinger/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.0
