# windows-taskbar-pinger

[![build](https://github.com/markpelayo/windows-taskbar-pinger/actions/workflows/build.yml/badge.svg)](https://github.com/markpelayo/windows-taskbar-pinger/actions/workflows/build.yml)
![Platform: Windows 10/11](https://img.shields.io/badge/platform-Windows%2010%2F11-lightgrey)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A tiny Windows taskbar app that pings a host and draws the result as a live grid of colored cells —
so you can tell at a glance whether your connection is healthy, flaky, or gone.

<img width="387" height="87" alt="image" src="https://github.com/user-attachments/assets/a7864783-1de8-4370-a64e-a57ecce9870a" />


Each cell is one ping: **blue = reply, red = no reply, dim = not measured yet.**

<img width="129" height="51" alt="image" src="https://github.com/user-attachments/assets/aee3935c-8a19-444b-8519-2c23672ec30f" />


This is a port of [macos-menubar-pinger](https://github.com/markpelayo/macos-menubar-pinger),
which was itself a macOS port of the idea behind the old
[Windows taskbar ping widget](https://superuser.com/questions/661132/show-current-ping-to-website-on-taksbar).
So it has come home.

Download it from [Releases](https://github.com/markpelayo/windows-taskbar-pinger/releases), unzip,
run. Nothing to install and nothing to configure.

## What it's for

You're on a call, or SSH'd into something, and the connection feels off. Was that a blip or is
your Wi-Fi actually dropping? Opening a terminal and running `ping` tells you what's happening
*now* — this tells you what's been happening for the last minute, permanently, out of the corner of
your eye.

Useful for:

- Spotting intermittent packet loss that a single `ping` run would miss
- Watching your home router and an external host side by side to see *where* the problem is
- Keeping an eye on a VPN, a lab machine, or a server during a deploy

Not a replacement for real monitoring — it's a glanceable indicator, and it only knows ICMP.

## How it works

**The newest ping is always the top-left cell.** Each older sample sits one place further along,
and the oldest falls off the far end when there is no room left. So the grid reads like a timeline
running away from the corner: what just happened is where you look, and history trails off behind
it.

By default cells run **across the top row, then down to the next** — the way text is read.
**Fill rows vertically** in the menu changes the path to go down the first column, then on to the
top of the next. Both start at the top-left cell and end at the bottom-right one; only the route
differs. Switching clears the grid, because a full one looks identical either way and the point of
changing direction is to watch where new cells land.

The average round trip across those same visible cells is printed next to the grid, and shown in
the menu; the readout can be switched off per monitor.

Everything is configurable from the right-click menu — target host, ping frequency, both colors,
and the grid's rows, columns, cell size and spacing. You can run up to 8 independent grids side by
side, one per host, each with its own settings. Settings can be saved as named **profiles** and
applied to any grid.

## Download

Grab the latest `Pinger-windows-x64.zip` from the
[Releases](https://github.com/markpelayo/windows-taskbar-pinger/releases) page, unzip it anywhere,
and run `Pinger.exe`. No installer, nothing to configure.

Want the build from the newest commit rather than the last release? The
[Actions](https://github.com/markpelayo/windows-taskbar-pinger/actions) tab has an artifact on
every run — open the most recent green one and download `Pinger-windows-x64`. (GitHub requires you
to be signed in to download artifacts.)

> **"Windows protected your PC."** SmartScreen shows this for any unsigned executable, and a
> code-signing certificate runs to several hundred dollars a year. Click **More info → Run anyway**,
> or build it yourself from source — code you compile locally is never flagged. If you would rather
> not do either, that is an entirely reasonable position; build it.

## Requirements

- Windows 10 (1607 or later) or Windows 11
- Nothing else to run it — the build is statically linked, so there is no runtime or
  redistributable to install.
- To build it yourself: **MSVC** — either Visual Studio 2022 Community or the standalone
  [Build Tools](https://visualstudio.microsoft.com/downloads/). Nothing else.

No package manager, no vcpkg, no NuGet, no third-party libraries. The RAII handle wrappers that
would normally come from Microsoft's WIL are about eighty lines in `src/raii.h`, kept in-tree
precisely so that cloning and building needs nothing but a compiler.

## Status

**v1.4.3. Working well on Windows 11** — daily use, left running, no known outstanding issues.

Everything in the menu reference below does what it says: the grid, the latency readout, multiple
monitors, profiles, the taskbar embedding, surviving an Explorer restart, and starting with Windows.

Two honest qualifications, neither of which is a reason not to use it:

**It has been proven on one configuration** — Windows 11, 100% scaling, a bottom-docked taskbar,
a single display. Everything else is untested rather than known-broken: Windows 10, fractional
scaling, a taskbar docked left or right, and multi-monitor setups. The code handles all of them
deliberately, but "handled in code" and "seen working" are different claims and only the second one
is worth much.

**The taskbar embedding is undocumented.** It relies on shell behaviour Microsoft has never
committed to, so a future Windows update could change it. That is a structural property of the
approach, not a bug waiting to be found — see [how the taskbar part works](#how-the-taskbar-part-works-and-what-that-costs).
The app falls back to a floating bar if embedding ever stops working, so the worst case is cosmetic.

Bug reports are welcome — see [CONTRIBUTING.md](CONTRIBUTING.md) for what to include. Reports from
the untested configurations above are the most useful kind.

## Build

Open **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu, then:

```bat
git clone https://github.com/markpelayo/windows-taskbar-pinger.git
cd windows-taskbar-pinger
build.bat
```

That produces `build\Pinger.exe`. Run it:

```bat
build\Pinger.exe
```

A small grid appears in your taskbar, already pinging `8.8.8.8`. There's no window and no taskbar
button — right-click the grid to configure it.

Prefer CMake? `CMakeLists.txt` is there too:

```bat
cmake -B build -S . -A x64
cmake --build build --config Release
```

### Start it at login

Right-click the grid, open **Run at startup**, and choose **On** — or a delay.

That writes a value under `HKEY_CURRENT_USER\...\CurrentVersion\Run`, which needs no administrator
rights and shows up in Task Manager's **Startup** tab, so it can be turned off from there too. Move
the folder later and the app corrects the stored path the next time it runs.

**About the delay.** Signing in to Windows is the busiest moment your disk and network will have all
day, and a ping widget has no business competing for it. Picking a delay of 5 to 60 seconds makes
the app wait that long before creating any window, touching the taskbar, resolving DNS or sending a
packet.

Be clear about what that does and does not do: Windows still *launches* the process at sign-in — a
Run entry cannot ask it not to. What the delay defers is everything the app actually costs. Truly
delaying the launch would need a Task Scheduler entry, which is a great deal more machinery than
this earns. In practice the deferral is where nearly all the benefit is.

If a policy on your machine locks that key, the app says so and you can fall back to the manual
route: press <kbd>Win</kbd>+<kbd>R</kbd>, enter `shell:startup`, and put a shortcut to `Pinger.exe`
in the folder that opens.

### Uninstalling

There's no installer, so there's nothing to uninstall. Close it from the menu, delete the folder,
and if you want to forget your saved monitors and profiles:

```bat
del "%APPDATA%\Pinger\settings.ini"
```

If you had **Run at startup** on, untick it before deleting the folder — or remove the leftover
entry afterwards:

```bat
reg delete "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v Pinger /f
```

## Quick start

1. Launch it. A grid appears in the taskbar, pinging `8.8.8.8` once per second.
2. Right-click the grid to open its menu.
3. **Set IP Address or Hostname…** → type the target you actually care about, e.g. `1.1.1.1`.
4. Watch. Blue cells mean replies. A red cell is a lost packet. A wall of red means you're offline.

Want a second host? **Duplicate this monitor**, then change the IP on the copy. The two grids run
completely independently.

## Menu reference

Right-click any grid:

| Item | What it does |
|---|---|
| **windows-taskbar-pinger …** | Name, version and author — click to open the project page |
| **Target: …** | The host this grid is pinging |
| **% reachable · latency** | Rolling stats for the cells currently visible, plus their average round trip |
| **Set IP Address or Hostname…** | Any IP or hostname; saved across launches (default `8.8.8.8`) |
| **Ping frequency ▸** | 0.5 s–60 s presets, or **Custom…** for anything from 0.25 s to an hour |
| **Show average latency** | Toggle: prints the average round trip next to the grid |
| **Latency text size ▸** | 8–16 pt (default 10); greyed out when the readout is off |
| **Success color ▸** | 10 presets plus **Custom…** (system color picker); default blue |
| **Failure color ▸** | Same, default red |
| **Fill rows vertically** | Toggle. Off (default): cells run across the top row, then down to the next. On: cells run down the first column, then on to the top of the next. Both start top-left and end bottom-right, and the newest ping is always the top-left cell either way. Switching empties the grid, like **Clear monitor history** |
| **Rows ▸** | 2–8 (default 4) |
| **Columns ▸** | 4–32 (default 8) |
| **Cell size ▸** | 2–12 px (default 6) |
| **Cell spacing ▸** | 0–3 px (default 1) |
| **Move widget…** | Arms dragging: the cursor changes, then drag the widget along the taskbar and release |
| **Reset widget position** | Back to the automatic spot beside the clock |
| **Ping now** | Force an immediate sample |
| **Clear monitor history** | Empty the grid |
| **Save monitor profile ▸** | **New profile…**, or pick a listed profile to overwrite it |
| **Load monitor profile ▸** | Apply a saved profile. Each row carries an ✕ on the right — click that instead to delete it. Greyed out when nothing is saved |
| **Restore monitor defaults** | Reset this grid to the factory settings. Greyed out while nothing has been changed, so it doubles as an indicator of whether this grid is customised. Affects only the grid you right-clicked — a note saying so appears once you have more than one |
| **Duplicate this monitor** | Add another grid with a copy of these settings |
| **Remove this monitor** | Delete just this grid (disabled when one is left) |
| **Run at startup ▸** | **Off**, **On**, or a delay of 5–60 seconds. Adds or removes an `HKEY_CURRENT_USER` Run entry — no admin rights, and visible in Task Manager's Startup tab. The parent row shows the current setting |
| **Quit Pinger** | |

Every setting persists across launches, per monitor, in `%APPDATA%\Pinger\settings.ini`.

The taskbar is short, so if `rows × (cell + spacing)` won't fit, cell **height** is shrunk to fit
while the width you picked is kept — the menu shows the fitted height when this happens.

## How the taskbar part works, and what that costs

This is the one genuinely unsupported thing the app does, so it's worth being straight about it.

macOS has `NSStatusItem`: a documented, arbitrary-width slot in the menu bar. **Windows has no
equivalent.** The notification area takes a fixed 16×16 icon and cannot display text, which would
lose both the wide grid and the latency readout. Deskbands — the old COM mechanism for real taskbar
toolbars — were deprecated in Windows 8 and their UI was removed in Windows 11.

What still works is to create an ordinary child window and re-parent it into the taskbar's own
window (`Shell_TrayWnd`) with `SetParent`. The child then moves, hides and auto-hides along with
the taskbar, because as far as the shell is concerned it *is* part of it. Shipping apps including
[FluentFlyout](https://github.com/unchihugo/FluentFlyout) and
[NetSpeedTray](https://github.com/erez-c137/NetSpeedTray) do the same.

Consequences you should know about:

- **It could break in future.** A Windows update that changes the shell's window structure could
  stop it working. It has been stable in practice on Windows 11, but nothing obliges Microsoft to
  keep it that way. When embedding fails the app falls back to a floating always-on-top bar
  positioned over the taskbar — less integrated, but never invisible.
- **Explorer restarts detach it.** Handled: the app listens for the shell's `TaskbarCreated`
  broadcast and re-embeds itself.
- **The taskbar doesn't announce moves.** There's no notification when it changes edge or size, so
  the app re-checks every two seconds. That's one `FindWindow` and one `GetWindowRect` — far below
  anything measurable.
- **The widget anchors to the notification area.** It parks immediately left of the clock and tray
  icons, and re-checks that position as the notification area changes width. It deliberately does
  *not* sit at the far left: that is where the weather and news widget lives, and the app buttons
  between them are centred on Windows 11 and move as windows open and close. The notification area
  is the only part of the taskbar that stays put.

  **Move widget…** in the menu overrides this if you want it somewhere else. Once you have moved
  it, the position is stored as a distance from the taskbar's right edge and stays exactly there —
  which means a tray that grows an icon will creep toward it. **Reset widget position** puts it
  back under automatic placement.

## Resource usage

It was written to be something you can leave running forever:

- **No child processes.** The macOS version parses the output of `ping -O`. Windows has
  `IcmpSendEcho` in `iphlpapi`, which sends an echo request from inside our own process — no
  `fork`/`exec`, no output parsing, no localised console text to scrape, and **no administrator
  rights**, unlike a hand-rolled raw ICMP socket.
- **Nothing is allocated on a repaint.** Brushes, fonts and the double-buffer bitmap are all cached
  and rebuilt only when something actually changes — a colour, the DPI, the widget's size. The GDI
  handle count is flat, which matters because a process is capped at 10,000 of them: leaking one per
  redraw would kill the app in under three hours.
- **Nothing is read from the registry or sent to another process on a repaint.** The taskbar's text
  colour is cached and refreshed on the theme-change broadcast rather than polled, and the tray
  tooltip — a cross-process call into Explorer — is rate limited rather than rewritten every second
  for text that is only visible while you hover.
- **No iostreams.** `<fstream>` and `<sstream>` were the single largest contributor to the binary,
  dragging in two full sets of locale facets that construct at startup and stay resident, for what
  amounts to reading and writing one small text file. Replaced with the Win32 file calls.
- **The taskbar poll uses a coalescable timer,** so Windows can batch its wakeup with other system
  activity instead of pulling an idle CPU out of sleep twice a second. That is the part that shows
  up in laptop battery life.
- **One small thread per monitor,** 64 KB of stack, blocked essentially all of its life. Not one
  thread per packet.
- **No GDI allocation per frame.** Brushes are cached and rebuilt only when a color actually
  changes; the back buffer is reused and reallocated only when the grid changes shape or DPI. This
  matters more than it sounds: a process is capped at 10,000 GDI handles, so leaking one brush per
  redraw would kill the app in under three hours. Every handle in the app is owned by an RAII
  wrapper in `src/raii.h` that frees it on scope exit.
- **Menus are built on open,** not kept live, so a ping never touches menu state.
- Settings are cached in memory and written through on change, so redraws do no file I/O.

Expect a few MB of RSS and effectively no CPU between packets.

## Limitations

- **IPv4 only.** `Icmp6SendEcho2` needs a bound source address and a different reply layout — a
  meaningful amount of extra code for a glanceable indicator. An IPv6-only target shows as
  unreachable rather than silently doing nothing.
- **One tooltip.** The notification-area icon shows the first monitor's stats. Per-grid hover
  tooltips would need a tracking control per grid.
- **A dragged position is anchored to the taskbar's right edge**, not to the notification area. It
  survives a resolution change, but tray icons appearing will encroach on it. Automatic placement
  (the default) does not have this problem.
- **Grid settings are per monitor; the widget's position and the startup entry are not.** Colours,
  size, fill direction and text size belong to one grid. Where the widget sits and whether it runs
  at startup apply to the whole app, even though both are reached from a grid's menu.
- **Only one instance runs at a time.** A second launch exits silently rather than stacking a
  duplicate widget into the taskbar.

## Troubleshooting

**Nothing appears.** The widget may be behind your pinned apps on a centered Windows 11 taskbar.
Look at the far left of the taskbar. Failing that, the notification-area icon is always there —
right-click it to get the menu.

**Everything is red.** Some networks block ICMP entirely. Try a different host before assuming
you're offline.

**A red cell right after launch.** Opening packets are routinely lost while the route is set up, so
the first result of a session is only drawn once it's unambiguous: a lone opening failure is held
back, and discarded if the next packet replies. If two fail in a row the host really is unreachable
and both cells are drawn.

**"Windows protected your PC" on launch.** SmartScreen flags unsigned executables. Click **More
info → Run anyway**, or build it yourself — code you compiled locally isn't flagged.

**It vanished after Explorer crashed.** It rebuilds itself when the shell comes back, normally
within a second or two. If Explorer never returns, quit from the notification icon and start it
again.

**The menu ignores a click.** Fixed in 1.2.2. Earlier builds only opened the menu when this process
happened to hold foreground rights, which made it look random — clicking a second time worked
because the first click granted them. Update if you are on 1.2.1 or older.

**The readout says `— ms`.** No reply has been measured yet in the cells currently on screen. It
appears at launch, after **Clear monitor history**, and after switching fill direction, and goes
away with the first successful ping.

## Licensing note

The taskbar embedding here was written from Microsoft's public documentation for `FindWindow`,
`SetParent` and `SetWindowPos`, **not** adapted from FluentFlyout or NetSpeedTray. Both of those
are GPL-3.0; copying their implementations would have forced this project to relicense. It stays
MIT.

## Disclaimer

**This software is provided free of charge, "as is", without warranty or support of any kind, and
is used entirely at your own risk.** It is a convenience indicator, not a monitoring tool, and must
not be relied upon where inaccuracy or failure could cause loss or harm. To the maximum extent
permitted by law the author accepts no liability of any kind arising from its use, including damage
to any machine, system or data.

It also relies on an undocumented shell behaviour to sit inside the taskbar, which Microsoft may
change or remove at any time.

## License

MIT © Mark Pelayo
