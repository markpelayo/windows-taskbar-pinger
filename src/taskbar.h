// taskbar.h — putting a window inside the Windows taskbar.
//
// This is the part with no supported API behind it, so it deserves an honest
// explanation.
//
// macOS gives you NSStatusItem: a documented, arbitrary-width slot in the menu
// bar. Windows has no equivalent. The notification area only accepts a fixed
// square icon (16x16 at 100% DPI) and cannot show text, which would lose both
// the wide grid and the latency readout. Deskbands, the old COM mechanism for
// real taskbar toolbars, were deprecated in Windows 8 and the UI for them was
// removed in Windows 11.
//
// What still works, and what shipping apps such as FluentFlyout and
// NetSpeedTray do, is to create an ordinary child window and re-parent it into
// the taskbar's own window (class name "Shell_TrayWnd") with SetParent. The
// child then moves, hides and auto-hides along with the taskbar, because as far
// as the shell is concerned it is part of it.
//
// This implementation was written from the public documentation for FindWindow,
// SetParent and SetWindowPos rather than adapted from either project: both are
// GPL-3.0 and this app is MIT, so copying their code would force a relicence.
//
// Being undocumented, embedding can fail — a future Windows update, a shell
// replacement, a locked-down machine. So every failure path falls back to a
// floating always-on-top window positioned over the taskbar. The widget is then
// slightly less integrated, but it is never invisible.

#pragma once

#include <windows.h>

namespace pinger {

// Where the widget ended up.
enum class HostMode {
    Embedded,   // a child of Shell_TrayWnd; moves and hides with the taskbar
    Floating    // topmost window positioned over the taskbar
};

// Which screen edge the taskbar is docked to.
enum class TaskbarEdge { Bottom, Top, Left, Right };

struct TaskbarInfo {
    HWND        window = nullptr;     // the Shell_TrayWnd itself
    RECT        bounds{};             // screen coordinates
    TaskbarEdge edge = TaskbarEdge::Bottom;
    int         dpi = 96;
    bool        valid = false;

    // The notification area ("TrayNotifyWnd"): the clock, the chevron, the
    // volume and network icons. The widget is parked immediately to its left,
    // which is the only part of the taskbar that stays put — the app buttons
    // are centred on Windows 11 and move as you open and close windows.
    RECT        notifyBounds{};
    bool        hasNotifyArea = false;
};

// Reads the taskbar's current position, size, edge and DPI.
TaskbarInfo QueryTaskbar();

// Height (or width, when docked left or right) available to a widget inside the
// taskbar, in physical pixels, with a small margin so cells do not touch the edge.
int UsableThickness(const TaskbarInfo& info);

// Attempts to make `child` a child of the taskbar window.
//
// Returns Embedded on success, Floating on any failure. On failure the caller
// should keep the window topmost and position it itself.
HostMode EmbedInTaskbar(HWND child, const TaskbarInfo& info);

// Detaches a previously embedded window, restoring it to a normal top-level
// window. Safe to call when not embedded.
void DetachFromTaskbar(HWND child);

// Places `child` at `offsetFromEdge` pixels along the taskbar.
//
// In Embedded mode the coordinates are relative to the taskbar window; in
// Floating mode they are screen coordinates. Callers do not need to care which.
void PositionWidget(HWND child,
                    const TaskbarInfo& info,
                    HostMode mode,
                    int offsetAlong,
                    int width,
                    int thickness);

// The colour the shell draws taskbar text in.
//
// GetSysColor(COLOR_BTNTEXT) is wrong here: it reports the *apps* theme, which
// is black under the default Windows 11 setup, while the taskbar itself follows
// the separate system theme and draws white. Reading the theme preference
// directly is the only way to match what is beside us.
COLORREF TaskbarTextColor();

// Registers for the "TaskbarCreated" broadcast, which the shell sends to every
// top-level window when Explorer restarts. That is the moment an embedded
// widget silently loses its parent and has to re-attach.
UINT RegisterTaskbarCreatedMessage();

}  // namespace pinger
