// taskbar.cpp — Shell_TrayWnd embedding, with a floating fallback.

#include "taskbar.h"

#include <algorithm>

namespace pinger {

namespace {

// GetDpiForWindow is Windows 10 1607 and later. Resolved dynamically so the app
// still starts on older builds, where it falls back to the system DPI.
using GetDpiForWindowFn = UINT(WINAPI*)(HWND);

int DpiForWindow(HWND window) {
    static GetDpiForWindowFn function = []() -> GetDpiForWindowFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return nullptr;
        return reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(user32, "GetDpiForWindow"));
    }();

    if (function && window) {
        const UINT dpi = function(window);
        if (dpi > 0) return static_cast<int>(dpi);
    }

    // Fall back to the primary screen's DPI.
    HDC screen = GetDC(nullptr);
    int dpi = 96;
    if (screen) {
        dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
    }
    return dpi > 0 ? dpi : 96;
}

TaskbarEdge EdgeFromBounds(const RECT& bounds) {
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;

    // A taskbar wider than it is tall is horizontal; which of the two horizontal
    // edges is decided by whether it sits in the top half of its monitor.
    if (width >= height) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTOPRIMARY);
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            const int monitorMiddle = (info.rcMonitor.top + info.rcMonitor.bottom) / 2;
            return bounds.top < monitorMiddle ? TaskbarEdge::Top : TaskbarEdge::Bottom;
        }
        return TaskbarEdge::Bottom;
    }

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromRect(&bounds, MONITOR_DEFAULTTOPRIMARY);
    if (monitor && GetMonitorInfoW(monitor, &info)) {
        const int monitorMiddle = (info.rcMonitor.left + info.rcMonitor.right) / 2;
        return bounds.left < monitorMiddle ? TaskbarEdge::Left : TaskbarEdge::Right;
    }
    return TaskbarEdge::Left;
}

}  // namespace

// -------------------------------------------------------------------- query

TaskbarInfo QueryTaskbar() {
    TaskbarInfo info;

    info.window = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!info.window) return info;

    if (!GetWindowRect(info.window, &info.bounds)) return info;

    // A zero-sized taskbar means auto-hide has it fully retracted, or the shell
    // is mid-restart. Either way there is nothing sensible to attach to yet.
    if (info.bounds.right <= info.bounds.left || info.bounds.bottom <= info.bounds.top) {
        return info;
    }

    info.edge = EdgeFromBounds(info.bounds);
    info.dpi = DpiForWindow(info.window);

    // The notification area is a direct child of the taskbar on every Windows
    // version that matters. If it is ever renamed or restructured we simply
    // fall back to a left-edge offset rather than failing.
    HWND notify = FindWindowExW(info.window, nullptr, L"TrayNotifyWnd", nullptr);
    if (notify && GetWindowRect(notify, &info.notifyBounds)) {
        if (info.notifyBounds.right > info.notifyBounds.left) {
            info.hasNotifyArea = true;
        }
    }

    info.valid = true;
    return info;
}

COLORREF TaskbarTextColor() {
    // HKCU\...\Themes\Personalize has two separate values. AppsUseLightTheme
    // governs application windows; SystemUsesLightTheme governs the shell —
    // the taskbar, the Start menu, the notification flyouts. We want the
    // second one, and the Windows 11 default is 0, meaning a dark taskbar with
    // white text.
    DWORD value = 0;
    DWORD size = sizeof(value);

    const LSTATUS status = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);

    if (status == ERROR_SUCCESS) {
        return value != 0 ? RGB(0, 0, 0) : RGB(255, 255, 255);
    }

    // The value is absent on older builds and on some managed profiles. White
    // is the safer default: it is what the shipped Windows 10 and 11 themes
    // use, and unreadable-dark-on-dark is a worse failure than the reverse.
    return RGB(255, 255, 255);
}

int UsableThickness(const TaskbarInfo& info) {
    if (!info.valid) return MulDiv(24, 96, 96);

    const bool horizontal =
        info.edge == TaskbarEdge::Top || info.edge == TaskbarEdge::Bottom;

    const int thickness = horizontal ? (info.bounds.bottom - info.bounds.top)
                                     : (info.bounds.right - info.bounds.left);

    // Leave a small margin top and bottom so the grid does not touch the edges.
    const int margin = std::max(2, MulDiv(4, info.dpi, 96));
    return std::max(4, thickness - margin * 2);
}

// ------------------------------------------------------------------ embedding

HostMode EmbedInTaskbar(HWND child, const TaskbarInfo& info) {
    if (!child || !info.valid || !info.window) return HostMode::Floating;

    // A window must be a child before SetParent will keep it clipped inside the
    // parent; without WS_CHILD it becomes an owned popup instead, which draws
    // outside the taskbar and does not move with it.
    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP);
    style |= WS_CHILD;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    // WS_EX_TOOLWINDOW keeps it out of Alt-Tab. Topmost is meaningless for a
    // child window and interferes with the shell's own z-ordering.
    LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
    exStyle &= ~static_cast<LONG_PTR>(WS_EX_TOPMOST);
    exStyle |= WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle);

    if (SetParent(child, info.window) == nullptr) {
        // Re-parenting refused. Put the styles back and float instead.
        DetachFromTaskbar(child);
        return HostMode::Floating;
    }

    // Force the frame change to take effect.
    SetWindowPos(child, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);

    return HostMode::Embedded;
}

void DetachFromTaskbar(HWND child) {
    if (!child) return;

    SetParent(child, nullptr);

    LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CHILD);
    style |= WS_POPUP;
    SetWindowLongPtrW(child, GWL_STYLE, style);

    LONG_PTR exStyle = GetWindowLongPtrW(child, GWL_EXSTYLE);
    exStyle |= WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    SetWindowLongPtrW(child, GWL_EXSTYLE, exStyle);

    SetWindowPos(child, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
}

// ----------------------------------------------------------------- placement

void PositionWidget(HWND child,
                    const TaskbarInfo& info,
                    HostMode mode,
                    int offsetAlong,
                    int width,
                    int thickness) {
    if (!child || !info.valid) return;

    const bool horizontal =
        info.edge == TaskbarEdge::Top || info.edge == TaskbarEdge::Bottom;

    const int barWidth = info.bounds.right - info.bounds.left;
    const int barHeight = info.bounds.bottom - info.bounds.top;

    int x = 0;
    int y = 0;
    int cx = width;
    int cy = thickness;

    if (horizontal) {
        cx = width;
        cy = thickness;
        x = offsetAlong;
        y = std::max(0, (barHeight - thickness) / 2);   // centre in the bar
    } else {
        // Docked left or right: the widget still draws horizontally, so it is
        // centred across the bar's width and offset down from the top.
        cx = std::min(width, barWidth);
        cy = thickness;
        x = std::max(0, (barWidth - cx) / 2);
        y = offsetAlong;
    }

    if (mode == HostMode::Floating) {
        // Screen coordinates rather than taskbar-relative ones.
        x += info.bounds.left;
        y += info.bounds.top;
    }

    SetWindowPos(child, mode == HostMode::Floating ? HWND_TOPMOST : nullptr, x, y, cx, cy,
                 mode == HostMode::Floating ? SWP_NOACTIVATE
                                            : SWP_NOACTIVATE | SWP_NOZORDER);
}

// -------------------------------------------------------------- shell restart

UINT RegisterTaskbarCreatedMessage() {
    // The shell broadcasts this to every top-level window when Explorer starts,
    // including after a crash. An embedded widget's parent is gone by then, so
    // this is the signal to re-embed and to re-add the notification icon.
    return RegisterWindowMessageW(L"TaskbarCreated");
}

}  // namespace pinger
