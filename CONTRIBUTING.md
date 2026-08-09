# Contributing

Issues and pull requests are welcome.

## The two rules that matter

**Keep the idle cost near zero.** This is something people leave running for weeks. Anything that
allocates per frame, wakes the CPU unnecessarily, or grows without bound will be asked about. In
particular: no GDI object should be created on a normal redraw. Brushes are cached and rebuilt only
when a colour actually changes, and there is a reason for that — a process is capped at 10,000 GDI
handles, so leaking one per redraw kills the app in under three hours.

**No dependencies.** `git clone` followed by `build.bat` should work with nothing but MSVC
installed. No vcpkg, no NuGet, no submodules. The RAII handle wrappers in `src/raii.h` exist
precisely so that Microsoft's WIL — which would be the obvious choice otherwise — is not needed.

## Building

Open **"x64 Native Tools Command Prompt for VS 2022"**, then:

```bat
build.bat          :: release
build.bat debug    :: with symbols
```

Or with CMake:

```bat
cmake -B build -S . -A x64
cmake --build build --config Release
```

Every push is built on Windows by GitHub Actions, so CI will catch it if a change breaks the build
on a clean machine.

Dependabot opens a weekly PR when the actions the workflow uses move — usually because GitHub has
retired the Node runtime an action declares, which shows up as a deprecation annotation on every
build until it is bumped. Those PRs build themselves before you merge them, so a green check means
the new versions work.

## Layout

```
src/main.cpp       entry point, single-instance guard
src/app.*          host window, layout, tray icon, message loop
src/monitor.*      one grid: its state, its menu, its per-packet logic
src/ping.*         ICMP via iphlpapi, one worker thread per monitor
src/grid.*         GDI rendering, the fill order, and the menu colour swatches
src/taskbar.*      Shell_TrayWnd embedding, with the floating fallback
src/settings.*     the model and its INI persistence
src/dialogs.*      text prompt, confirmation, colour picker
src/autostart.*    the HKCU Run entry behind "Run at startup"
src/raii.h         scoped handle wrappers — read this before touching GDI
src/defaults.h     factory settings and the values the menus offer
```

## Style

- Follow what is already there. Four spaces, `PascalCase` for functions and types, `camelCase_` for
  members.
- **Comment the why, not the what.** `// increment the counter` above `++counter` is noise.
  Explaining why a wedged ping thread is deliberately leaked rather than joined is not.
- Prefer a clear explanatory comment over a clever line of code.

## Things worth knowing before you change them

**The taskbar embedding is undocumented.** `src/taskbar.cpp` re-parents the widget into
`Shell_TrayWnd`, which is not a supported API. It was written from Microsoft's public documentation
for `FindWindow`, `SetParent` and `SetWindowPos` rather than adapted from FluentFlyout or
NetSpeedTray — both are GPL-3.0, and copying their implementations would force this project to
relicense away from MIT. **Please keep it that way.** If you are fixing embedding, work from the
Win32 docs, not from someone else's GPL source.

**The first-packet hold-back is deliberate.** A lone opening failure is held back rather than drawn,
because opening packets are routinely lost to route setup. If two fail in a row, both are drawn.
This is re-armed on every session, not just the first — see `MonitorController::Restart`.

**Ping worker threads post results by index.** Monitor indices are renumbered when one is removed,
and `MonitorController::SetIndex` forwards that to the session. Getting this wrong causes one grid
to freeze and another to display someone else's host, which is not obvious from a quick test.

**Everything a ping worker touches lives in a refcounted `PingShared`,** not in `PingSession`
itself. That is what makes it safe for `Stop()` to give up waiting on a wedged worker: the thread
co-owns the block, so it can never dereference freed memory. Do not move worker-visible state back
onto the session object.

**Menus and dialogs are owned by `App::MenuOwner()`, never by the widget.** The widget is a
`WS_CHILD` of `Shell_TrayWnd`, and a modal dialog disables its owner's top-level ancestor — which
would be the taskbar. `ForceForeground` in `monitor.cpp` exists because `SetForegroundWindow` fails
from a process the user has not activated, and clicking a child of the taskbar does not activate
this one; the input-queue attachment is what makes it succeed. Removing it brings back the
"first click does nothing" bug.

**`TrackPopupMenuEx` runs its own message pump.** Anything posted to the widget can be dispatched
while a menu is open, which is why `MenuIsOpen()` guards both nested menus and the deferred monitor
removal.

**Nothing may be allocated on a repaint.** Brushes, fonts, the back-buffer bitmap and the swatch
cache are all built once and reused; the theme colour is cached rather than read from the registry.
A process is capped at 10,000 GDI handles, so one leaked object per redraw kills the app in under
three hours.

## Reporting a bug

Include your Windows version and build (`winver`), whether the widget embedded in the taskbar or
fell back to floating, your display scaling, and the contents of
`%APPDATA%\Pinger\settings.ini`.
