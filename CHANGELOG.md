# Changelog

All notable changes to this project are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

Nothing yet.

## [1.1.0] — 2026-08-09

A full audit pass for correctness, leaks and footprint. No new features; the
version bumps to 1.1.0 because the internals changed substantially.

### Fixed

- **A use-after-free in the ping worker.** If a worker ever failed to stop
  within the timeout, the code deliberately abandoned it — but then destroyed
  the object the worker was still reading, including an interlocked write to a
  freed heap slot, which is corruption rather than merely a stale read. The
  worker's state now lives in a refcounted block the thread co-owns, so
  abandoning one is safe. As a side effect the "abandoned" flag is gone, and a
  monitor whose worker wedged is no longer disabled for the rest of the session.

- **Explorer restarting killed the app.** Once embedded, the widget is a child
  of `Shell_TrayWnd`, so the shell destroys it along with the taskbar. The
  `WM_DESTROY` handler treated that as the user quitting and ended the message
  loop — meaning the `TaskbarCreated` recovery path added in 1.0.1 could never
  actually run. The widget is now rebuilt, with a timer as a fallback in case
  the broadcast never arrives.

- **One busy moment could freeze a monitor permanently.** `PostMessage` fails
  with a full queue as well as on a dead window, and both were treated as fatal
  to the worker. Only an invalid window handle retires it now; a full queue is
  transient and the next packet gets through.

- **Modal dialogs disabled the whole taskbar.** They were owned by the widget,
  and Windows resolves a child window's owner to its top-level ancestor — which
  here is the taskbar itself. They now use the same hidden owner window the
  menus use.

- **`ReleaseCapture` was called from inside `WM_CAPTURECHANGED`,** which MSDN
  forbids and which could re-enter the handler and write the widget position
  twice.

- **A latency average sitting on a digit boundary** — 9.5 ms, say, which is an
  utterly ordinary gateway figure — flipped between `9 ms` and `10 ms` and
  triggered a full relayout, including a `SetWindowPos` on a child of the
  taskbar, potentially every second. The reserved width now grows immediately
  but only shrinks once the shorter reading has held for ten samples.

- Queued ping results are drained at shutdown and when the shell destroys the
  widget, instead of being discarded with their heap payloads.

- The taskbar-thickness fallback was `MulDiv(24, 96, 96)` — that is, 24 at any
  scaling.

- The menu font is rebuilt on a DPI or theme change, so the owner-drawn profile
  rows no longer stay at the old size while everything around them scales.

- **The CMake build produced a `/MD` binary** that needs the Visual C++
  redistributable installed, contradicting both the README and the release
  notes. It now matches `build.bat` and links the CRT statically. The shipped
  artifact was always correct, since CI runs `build.bat`; anyone building
  through CMake was getting a more fragile executable.

### Changed — footprint

- **`<fstream>` and `<sstream>` are gone.** Between them they instantiated file
  and string streams, both narrow and wide, and two complete sets of locale
  facets that constructed at startup and stayed resident — by a wide margin the
  largest single contributor to the binary. The settings code already did its
  own UTF-8 conversion and BOM handling, so the streams were contributing
  nothing but byte transport; they are replaced by `CreateFile`/`ReadFile`/
  `WriteFile` and a line splitter.

- **The back buffer is no longer rebuilt every frame.** A memory DC, a bitmap
  and a brush were created and destroyed on every repaint — three GDI objects a
  second, forever. Nothing leaked, but the handle count visibly oscillated in
  Task Manager, and it contradicted the file's own claim that a redraw allocates
  nothing. They are cached and rebuilt only when the widget changes size.

- **The theme colour is no longer read from the registry on every frame.** It
  was a `RegGetValueW` per monitor per second for a value that changes only on a
  theme switch, which the app already receives a broadcast for.

- **The tray tooltip no longer updates once a second.** `Shell_NotifyIcon` is a
  cross-process call into Explorer, and this text is only ever read while the
  pointer rests on the icon — so the app was waking another process every second
  to update something nobody was looking at. Rate limited to once every five
  seconds.

- **The taskbar poll uses a coalescable timer** where available, letting Windows
  batch the wakeup with other system activity rather than pulling an idle CPU
  out of sleep on its own schedule. This is the part that matters for laptop
  battery; the poll's CPU cost was always negligible.

- The swatch cache is a small fixed array rather than a `std::map`, and the ping
  reply buffer is on the stack rather than heap-allocated. `<map>` is out of the
  binary entirely.

- Build flags gain `/Gw` and `/Zc:inline`, which let the existing `/OPT:REF`
  discard unreferenced *data* and not just code. `/GS` stays on deliberately:
  this app parses a user-editable file and formats network data into fixed
  buffers, which is exactly what the stack cookie protects.

### Deliberately not changed

- **Exceptions and RTTI stay enabled.** Disabling exceptions is an unsupported
  STL configuration and would turn an allocation failure into an abort; RTTI
  costs almost nothing here because there are no virtual functions.
- **One thread per monitor stays.** Sharing one would save around 100 KB at the
  eight-monitor maximum and nothing at all at the default of one, in exchange
  for a substantially more complicated scheduler.
- **The taskbar poll stays.** There is no notification for the notification area
  changing width, which is the case it exists to catch.


## [1.0.5] — 2026-08-09

### Added

- **Fill direction is switchable,** from a new toggle beneath **Failure color**.

  Off, the default, keeps the order inherited from the macOS version: cells climb a column from the
  bottom-left, then move to the next column, with the newest sample at the top right.

  On, cells run left to right along the top row and then down — the way text is read — with the
  newest sample at the bottom right.

  Only the mapping from sample to cell position changes. The same samples occupy the same number of
  cells, the rolling window behaves identically, and the average latency is computed from the same
  set either way, so switching direction re-flows the existing history rather than disturbing it.

## [1.0.4] — 2026-08-09

### Fixed

- **The menu often ignored the first click and only opened on the second.** `TrackPopupMenu`
  requires its owner window to be the foreground window, and `SetForegroundWindow` fails silently on
  a `WS_CHILD` — which the widget is, being parented into the taskbar. Menus are now owned by a
  hidden top-level window created for the purpose, which can legitimately be foregrounded.

### Changed

- **Saved profiles can be deleted from the row itself.** Each row in **Load monitor profile** now
  carries an ✕ on the right; clicking it asks for confirmation and removes the profile, while
  clicking anywhere else on the row loads it as before. This restores the affordance the macOS
  version had, and replaces the separate **Delete profile** submenu.

  Win32 menu items cannot host child controls the way the macOS `ProfileRowView` did, so the rows
  are owner-drawn and the click is resolved by comparing the cursor against the row's on-screen
  rectangle — captured while the row is still highlighted, since the menu is destroyed before the
  command is delivered.

- **Load monitor profile is greyed out when nothing is saved,** instead of opening a submenu whose
  only entry says "No saved profiles". Making someone travel into a menu to discover it is empty is
  a small rudeness the disabled item avoids.

## [1.0.3] — 2026-08-09

### Added

- **Latency text size is configurable**, from the new **Latency text size** submenu directly beneath
  **Show average latency**. Offers 8 to 16 pt, and is greyed out when the readout is hidden, since
  there is nothing for it to size.

  Per-monitor rather than app-wide, like every other visual setting, so two grids side by side can
  be weighted differently — and because a global setting reached from a per-monitor menu would be
  misleading about what it affects.

### Changed

- **The readout is 10 pt by default, down from 12.** 12 pt was noticeably oversized against a 6 px
  grid in a taskbar. The clock beside it is 9 pt; one point above that reads as lightly emphasised
  without dominating. Anyone who preferred the larger size can set it back in the new submenu.

### Internal

- Dependabot now watches the GitHub Actions used by the build workflow and opens a grouped weekly
  PR when they move. Actions pin a Node runtime in their own metadata and GitHub retires those
  runtimes on its own schedule, so without this every build eventually prints a deprecation
  annotation until someone notices. No effect on the binary.

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
- **Profile deletion was a submenu** rather than an ✕ button on each row. Win32 menu items cannot host
  child controls the way the macOS `ProfileRowView` did. (Owner-drawn rows restored the ✕ in 1.0.4.)
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

[Unreleased]: https://github.com/markpelayo/windows-taskbar-pinger/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.1.0
[1.0.5]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.5
[1.1.0]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.1.0
[1.0.5]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.5
[1.0.4]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.4
[1.0.3]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.3
[1.0.2]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.2
[1.0.1]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.1
[1.0.0]: https://github.com/markpelayo/windows-taskbar-pinger/releases/tag/v1.0.0
