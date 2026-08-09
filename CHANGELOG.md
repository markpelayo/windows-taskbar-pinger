# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.0.2] — 2026-08-09

### Fixed

- **A large dead gap appeared between the latency readout and the notification area** whenever
  **Show average latency** was on. The widget reserved a fixed worst-case width for the text — wide
  enough for `9999 ms` — and since the widget's right edge is where the *reserved space* ends
  rather than where the text does, a short reading like `5 ms` left roughly 40 px of nothing before
  the tray.

  The readout is now measured. Not of the live string, though: the widget is anchored by its right
  edge, so sizing to the exact text would shift the grid sideways on every change, and at one
  update a second that is a visible twitch. Instead it measures a template of the same length with
  each digit replaced by `8`, the widest figure. Width then depends only on the number of
  characters, so it is stable through `5 ms` → `6 ms` and only re-measures across `9` → `10` or
  `99` → `100`.

### Added

- **The widget can be moved along the taskbar.** **Move widget…** in the menu arms dragging — the
  cursor changes to a resize arrow — then you drag it and release. **Reset widget position** returns
  it to automatic placement beside the clock.

  Dragging is menu-armed rather than always available on purpose: the widget lives in the taskbar,
  where an accidental drag would be easy and a status indicator that has quietly wandered off is
  irritating to find again.

  A moved position is stored as a distance from the taskbar's right edge, in DPI-independent units,
  so it survives a resolution change and clamps rather than disappearing if the new screen is
  narrower. The trade-off is that it is anchored to the taskbar edge and not to the notification
  area, so tray icons appearing will gradually encroach on it — automatic placement, which tracks
  the notification area, does not have that problem.

## [1.0.1] — 2026-08-09

Everything user-visible that was wrong with 1.0.0 on a default Windows 11 desktop. That release was
tagged from a build that had never been run — the port was written on a Mac — and these four turned
up within minutes of it starting for the first time.

**If you have 1.0.0, replace it.** Nothing in it is dangerous, but the latency readout is invisible
and the grid sits on top of the weather widget.

### Fixed

- **The widget landed on top of the Windows 11 weather and news widget.** It was placed at a fixed
  offset from the taskbar's left edge, which is exactly where that widget lives. It now anchors to
  the notification area — the only region of the taskbar that stays put, since the app buttons are
  centred and shift as windows open and close. The offset is recomputed on every layout, and the
  tray's bounds are watched so the widget follows when an icon appears or the clock changes width.
- **The latency readout was black on a dark taskbar,** so effectively invisible. It was drawn with
  `GetSysColor(COLOR_BTNTEXT)`, which reports the *apps* theme; Windows tracks the shell's theme
  separately, and the shipped default is light apps on a dark taskbar. It now reads
  `SystemUsesLightTheme`, falling back to white when that value is absent.
- **The readout was too small to read** at 9 pt beside a 6 px grid. It is now 12 pt, with the
  reserved width widened to match.
- **The about menu item opened the macOS repository** instead of this one.

### Changed

- Default grid is 4 rows, up from 3.

### Known issues

- The widget is not draggable; its position is computed rather than chosen. (Added in 1.0.2.)
- Tested on Windows 11 at 100% scaling with a bottom-docked taskbar. Other versions, scalings,
  taskbar positions and multi-monitor setups are lightly tested at best.
- If `rows × (cell + spacing)` exceeds the taskbar height, cell height is shrunk to fit and the
  grid looks squashed rather than square. Reduce **Cell size** if the default 4 rows does this.

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

### Known issues

**Superseded by 1.0.1 — use that instead.** This release was tagged from code that had never been
run. The latency readout is drawn in black on the dark taskbar and is effectively invisible, and
the widget is positioned on top of the Windows 11 weather and news widget.

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

[Unreleased]: https://github.com/markpelayo/windows-taskbar-pinger/compare/v1.0.2...HEAD
[1.0.2]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.2
[1.0.1]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.1
[1.0.0]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.0
