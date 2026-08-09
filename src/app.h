// app.h — the process: the host window, the monitors, the tray icon.
//
// One window hosts every grid side by side, rather than one window per monitor.
// The taskbar is a single parent and the grids are always laid out in a row, so
// a single window means one embed to maintain, one paint, and one place that
// knows the layout — instead of eight of each.

#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

#include "monitor.h"
#include "raii.h"
#include "taskbar.h"

namespace pinger {

class App {
public:
    App();
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Creates the host window, loads monitors and attaches to the taskbar.
    // Returns false when the window could not be created at all.
    bool Initialise(HINSTANCE instance);

    // Runs the message loop until the user quits.
    int Run();

    HWND HostWindow() const { return window_; }
    int Dpi() const { return dpi_; }
    int AvailableThickness() const { return availableThickness_; }

    int MonitorCount() const { return static_cast<int>(monitors_.size()); }

    void DuplicateMonitor(MonitorController* source);

    // Requests removal. Deliberately deferred via a posted message: this is
    // called from inside the target's own menu handler, so destroying it here
    // would unwind back through a freed object.
    void RemoveMonitor(MonitorController* target);

    // Recomputes every widget's width and the host window's size, then repaints.
    // Called whenever a grid changes shape or the taskbar moves.
    void Relayout();

    void RequestQuit();

    // Arms dragging. The next left-press on the widget starts a move, and the
    // release ends it and saves the new position.
    //
    // Deliberately menu-armed rather than always-on: the widget sits in the
    // taskbar, where a stray drag would be easy and the consequence — a status
    // indicator that has quietly wandered off — is annoying to undo.
    void BeginMoveMode();
    bool IsMoveMode() const { return moveMode_; }

    // Back to the computed position beside the notification area.
    void ResetWidgetPosition();

    // The window that owns popup menus.
    //
    // Not the widget itself. TrackPopupMenu needs its owner to be the
    // foreground window or the menu will not take focus, and SetForegroundWindow
    // silently fails on a WS_CHILD — which the widget is, being parented into
    // the taskbar. The symptom is a menu that ignores the first click and only
    // opens on the second. So menus are owned by this hidden top-level window,
    // which can legitimately be foregrounded.
    HWND MenuOwner() const { return menuOwner_; }

    // Registers the profile names an owner-drawn menu is about to display, so
    // the draw handler can find the text by item index.
    void SetProfileMenuNames(std::vector<std::wstring> names);

    // True when the selection that just closed a menu landed on the delete
    // glyph at the right of a profile row rather than on the row itself.
    bool LastSelectionHitDeleteGlyph() const;

    // Forgets any captured row rectangle. Called before showing a menu so a
    // rect left over from a previous one cannot be mistaken for this one's.
    void ClearMenuSelection();

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResult(unsigned monitorIndex, PingResult* result);
    void OnRightClick(POINT screenPoint);
    void OnTaskbarCreated();
    void OnDpiChanged();

    // Returns true when the message was consumed by an in-progress drag.
    bool OnDragButtonDown();
    bool OnDragMouseMove();
    // `releaseCapture` must be false when called from WM_CAPTURECHANGED: capture
    // is already gone by then, and MSDN forbids releasing it from that handler.
    bool OnDragButtonUp(bool releaseCapture);

    // Is the taskbar horizontal? Dragging follows its long axis either way.
    bool TaskbarIsHorizontal() const;

    // Performs the removal requested by RemoveMonitor, once the menu handler
    // that asked for it has returned. Acts on pendingRemovalId_; the parameter
    // is unused and kept only to match the posted message's shape.
    void RemoveMonitorAt(unsigned index);

    // Which monitor's grid contains this client-area x coordinate.
    MonitorController* MonitorAtPoint(POINT clientPoint);

    // Distance along the taskbar at which to place the widget, chosen so its
    // trailing edge sits just before the notification area.
    int ComputeOffsetAlong(int widgetWidth) const;

    void AttachToTaskbar();
    void PersistMonitors();
    void UpdateTooltip();

    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);

    void OnMeasureProfileItem(MEASUREITEMSTRUCT* measure);
    void OnDrawProfileItem(DRAWITEMSTRUCT* draw);
    void OnMenuSelect(WPARAM wParam, LPARAM lParam);
    HFONT MenuFont();

    HINSTANCE instance_ = nullptr;
    HWND      window_ = nullptr;

    // See MenuOwner().
    HWND menuOwner_ = nullptr;

    // Names backing the owner-drawn profile rows, indexed by item data.
    std::vector<std::wstring> profileMenuNames_;

    // Screen rect of the row highlighted when a menu closed, captured from
    // WM_MENUSELECT because the menu is gone by the time the command arrives.
    RECT lastMenuItemRect_{};
    bool lastMenuItemValid_ = false;

    ScopedFont menuFont_;
    int        menuFontDpi_ = 0;

    // Cached paint resources.
    //
    // These used to be created and destroyed inside every WM_PAINT — a memory
    // DC, a bitmap and a brush, three GDI objects a second, forever. Nothing
    // leaked, but the handle count visibly oscillated in Task Manager and it
    // contradicted this file's own claim that a redraw allocates nothing. The
    // buffer is now rebuilt only when the widget's size changes.
    ScopedMemoryDC bufferDc_;
    ScopedBitmap   buffer_;
    int            bufferWidth_ = 0;
    int            bufferHeight_ = 0;
    ScopedBrush    chromaBrush_;

    // The shell's text colour, refreshed on WM_SETTINGCHANGE rather than read
    // from the registry on every frame.
    COLORREF taskbarTextColor_ = RGB(255, 255, 255);

    // Rate limit for the tray tooltip, which is a cross-process call into
    // Explorer and only ever seen while hovering.
    ULONGLONG lastTooltipUpdate_ = 0;

    std::vector<std::unique_ptr<MonitorController>> monitors_;

    TaskbarInfo taskbar_;
    HostMode    hostMode_ = HostMode::Floating;
    int         dpi_ = 96;
    int         availableThickness_ = 24;
    // How far along the taskbar the widget sits. Positive from the left edge.
    int         offsetAlong_ = 0;


    // Dragging. `moveMode_` is armed from the menu and cleared when the drag
    // finishes; `dragging_` is true only between the press and the release.
    bool  moveMode_ = false;
    bool  dragging_ = false;
    POINT dragStart_{};
    int   dragStartOffset_ = 0;

    // The widget's overall width, kept from the last layout so a drag can clamp
    // against the taskbar without recomputing everything on every mouse move.
    int lastTotalWidth_ = 0;

    // Creates the widget window and applies its layered colour key. Called at
    // startup and again if the shell destroys it during an Explorer restart.
    bool CreateWidgetWindow();

    // Starts the taskbar poll, preferring a coalescable timer where available.
    void StartTaskbarPoll();

    UINT taskbarCreatedMessage_ = 0;
    bool trayIconAdded_ = false;

    // True once the user has asked to quit, so WM_DESTROY can tell a deliberate
    // shutdown from the shell tearing the widget down underneath us.
    bool quitting_ = false;

    // Which monitor a posted WM_PINGER_REMOVE refers to. Held as an id rather
    // than an index because indices are renumbered on every removal.
    std::wstring pendingRemovalId_;
};

}  // namespace pinger
