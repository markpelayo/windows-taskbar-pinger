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

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void OnPaint();
    void OnResult(unsigned monitorIndex, PingResult* result);
    void OnRightClick(POINT screenPoint);
    void OnTaskbarCreated();
    void OnDpiChanged();

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
    void EnsureFont();
    void UpdateTooltip();

    bool AddTrayIcon();
    void RemoveTrayIcon();
    void ShowTrayMenu(POINT screenPoint);

    HINSTANCE instance_ = nullptr;
    HWND      window_ = nullptr;

    std::vector<std::unique_ptr<MonitorController>> monitors_;

    TaskbarInfo taskbar_;
    HostMode    hostMode_ = HostMode::Floating;
    int         dpi_ = 96;
    int         availableThickness_ = 24;
    // How far along the taskbar the widget sits. Positive from the left edge.
    int         offsetAlong_ = 0;

    ScopedFont  font_;
    int         fontDpi_ = 0;

    UINT taskbarCreatedMessage_ = 0;
    bool trayIconAdded_ = false;

    // Which monitor a posted WM_PINGER_REMOVE refers to. Held as an id rather
    // than an index because indices are renumbered on every removal.
    std::wstring pendingRemovalId_;
};

}  // namespace pinger
