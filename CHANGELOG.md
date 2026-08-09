# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.0.0] — 2026-08-09

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

### Fixed before release

Found on the first run on real hardware, after the port was written blind on a Mac:

- **The widget landed on top of the Windows 11 weather and news widget.** It was placed at a fixed
  offset from the taskbar's left edge, which is exactly where that widget lives. It now anchors to
  the notification area — the only region of the taskbar that stays put, since the app buttons are
  centred and shift as windows open and close. The offset is recomputed on every layout, and the
  tray's bounds are watched so the widget follows when an icon appears or the clock changes width.
- **The latency readout was black on a dark taskbar,** effectively invisible. It was drawn with
  `GetSysColor(COLOR_BTNTEXT)`, which reports the *apps* theme; Windows tracks the shell's theme
  separately, and the default is light apps on a dark taskbar. It now reads `SystemUsesLightTheme`,
  falling back to white when the value is absent.
- **The readout was too small to read** at 9 pt beside a 6 px grid; it is now 12 pt, with the
  reserved width widened to match.
- Default grid changed from 3 rows to 4.

### Known issues

- The widget is not draggable; its position is computed rather than chosen.
- Tested on Windows 11 at 100% scaling with a bottom-docked taskbar. Other versions, scalings,
  taskbar positions and multi-monitor setups are lightly tested at best.
- If `rows × (cell + spacing)` exceeds the taskbar height, cell height is shrunk to fit and the
  grid looks squashed rather than square. Reduce **Cell size** if the default 4 rows does this.

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
