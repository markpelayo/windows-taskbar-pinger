// app.cpp — host window, layout, tray icon, message loop.

#include "app.h"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cwchar>

namespace pinger {

namespace {

constexpr wchar_t kWindowClass[] = L"PingerTaskbarWidget";
constexpr wchar_t kWindowTitle[] = L"Pinger";

// Notification-area icon identity and its callback message.
constexpr UINT kTrayIconId = 1;
constexpr UINT WM_PINGER_TRAY = WM_APP + 2;
constexpr UINT WM_PINGER_REMOVE = WM_APP + 3;

// Colour key for the layered window. Any pixel painted exactly this colour is
// transparent, which is how the taskbar shows through behind the grid.
//
// A near-black value rather than a garish magenta: text is antialiased against
// whatever it is drawn over, and edge pixels that do not match the key exactly
// stay opaque. Blending toward near-black is invisible on the default dark
// taskbar and unobtrusive on a light one; magenta would fringe every glyph.
constexpr COLORREF kChromaKey = RGB(1, 1, 1);

// Gap between adjacent grids, in logical pixels.
constexpr int kMonitorGap = 10;

// Gap between the widget's right edge and the notification area, in logical
// pixels. Enough to read as a separate thing rather than crowding the chevron.
constexpr int kNotifyAreaGap = 12;

// Fallback distance from the left edge, used only when the notification area
// cannot be found.
constexpr int kLeftEdgeFallback = 8;

// Re-checks the taskbar's position and size. The shell does not notify us when
// the taskbar moves or resizes, so this is a low-frequency poll; two seconds is
// far below what anyone notices and costs one FindWindow plus one GetWindowRect.
constexpr UINT kTaskbarPollTimerId = 1;
constexpr UINT kTaskbarPollIntervalMs = 2000;

}  // namespace

// ------------------------------------------------------------------ lifetime

App::App() = default;

App::~App() {
    RemoveTrayIcon();

    // Controllers must die before the window they post results to.
    monitors_.clear();
    ClearSwatchCache();

    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool App::Initialise(HINSTANCE instance) {
    instance_ = instance;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    // Redraw the whole client area on resize; the grid is repositioned, not
    // scrolled, so a partial redraw would leave stale cells behind.
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &App::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // No background brush: everything is painted in WM_PAINT, and letting the
    // system erase first would flash the taskbar's colour on every packet.
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;

    if (!RegisterClassExW(&windowClass)) {
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }

    taskbarCreatedMessage_ = RegisterTaskbarCreatedMessage();

    // Created as a popup first, then converted to a child if embedding works.
    // WS_EX_LAYERED gives us the colour key that lets the taskbar show through;
    // without it the widget would sit on an opaque rectangle, and GDI cannot
    // reproduce the Windows 11 acrylic behind it.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                              kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 100, 24,
                              nullptr, nullptr, instance, this);

    if (!window_) return false;

    SetLayeredWindowAttributes(window_, kChromaKey, 0, LWA_COLORKEY);

    taskbar_ = QueryTaskbar();
    dpi_ = taskbar_.valid ? taskbar_.dpi : 96;
    availableThickness_ = UsableThickness(taskbar_);

    for (const MonitorRecord& record : store::LoadMonitors()) {
        if (monitors_.size() >= static_cast<size_t>(defaults::kMaxMonitors)) break;
        monitors_.push_back(std::make_unique<MonitorController>(
            this, record, window_, static_cast<unsigned>(monitors_.size())));
    }

    // A settings file with no usable monitors would leave nothing on screen.
    if (monitors_.empty()) {
        MonitorRecord fresh;
        fresh.id = L"default";
        monitors_.push_back(std::make_unique<MonitorController>(this, fresh, window_, 0));
        PersistMonitors();
    }

    AttachToTaskbar();
    EnsureFont();
    Relayout();

    ShowWindow(window_, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    SetTimer(window_, kTaskbarPollTimerId, kTaskbarPollIntervalMs, nullptr);
    return true;
}

int App::Run() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

void App::RequestQuit() { PostQuitMessage(0); }

// -------------------------------------------------------------------- moving

void App::BeginMoveMode() {
    moveMode_ = true;

    // The cursor only updates on the next mouse move, so nudge it now — this is
    // the sole feedback that the mode is armed.
    POINT cursor{};
    if (GetCursorPos(&cursor)) SetCursorPos(cursor.x, cursor.y);
}

void App::ResetWidgetPosition() {
    WidgetPlacement placement;
    placement.manual = false;
    placement.offsetFromRight = 0;
    store::PersistPlacement(placement);

    moveMode_ = false;
    dragging_ = false;
    Relayout();
}

bool App::OnDragButtonDown() {
    if (!moveMode_ || dragging_) return false;
    if (!GetCursorPos(&dragStart_)) return false;

    dragging_ = true;
    dragStartOffset_ = offsetAlong_;
    SetCapture(window_);
    return true;
}

bool App::OnDragMouseMove() {
    if (!dragging_) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return true;

    const bool horizontal = TaskbarIsHorizontal();
    const int delta = horizontal ? (cursor.x - dragStart_.x) : (cursor.y - dragStart_.y);

    const int barLength = horizontal ? (taskbar_.bounds.right - taskbar_.bounds.left)
                                     : (taskbar_.bounds.bottom - taskbar_.bounds.top);
    const int extent = horizontal ? lastTotalWidth_ : availableThickness_;

    offsetAlong_ = std::clamp(dragStartOffset_ + delta, 0, std::max(0, barLength - extent));

    PositionWidget(window_, taskbar_, hostMode_, offsetAlong_, lastTotalWidth_,
                   availableThickness_);
    return true;
}

bool App::OnDragButtonUp() {
    if (!dragging_) return false;

    ReleaseCapture();
    dragging_ = false;
    moveMode_ = false;   // one drag per arming

    const bool horizontal = TaskbarIsHorizontal();
    const int barLength = horizontal ? (taskbar_.bounds.right - taskbar_.bounds.left)
                                     : (taskbar_.bounds.bottom - taskbar_.bounds.top);
    const int extent = horizontal ? lastTotalWidth_ : availableThickness_;

    // Convert back to a DPI-independent distance from the trailing edge.
    const int fromEnd = barLength - offsetAlong_ - extent;

    WidgetPlacement placement;
    placement.manual = true;
    placement.offsetFromRight = MulDiv(std::max(0, fromEnd), 96, dpi_);
    store::PersistPlacement(placement);

    return true;
}

// -------------------------------------------------------------------- window

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    App* app = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        if (app) app->window_ = window;
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (app) return app->HandleMessage(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT App::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // The shell broadcasts this when Explorer restarts, which is exactly when an
    // embedded widget has silently lost its parent.
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        OnTaskbarCreated();
        return 0;
    }

    switch (message) {
        case WM_PAINT:
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            // Claimed so the system does not paint over the taskbar behind us,
            // which would flicker once a second.
            return 1;

        case WM_PINGER_RESULT: {
            auto* result = reinterpret_cast<PingResult*>(lParam);
            OnResult(static_cast<unsigned>(wParam), result);
            delete result;   // ownership was handed over by the worker thread
            return 0;
        }

        case WM_PINGER_REMOVE:
            RemoveMonitorAt(0);
            return 0;

        case WM_PINGER_TRAY:
            if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_LBUTTONUP) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ShowTrayMenu(cursor);
            }
            return 0;

        case WM_LBUTTONDOWN:
            if (OnDragButtonDown()) return 0;
            return 0;

        case WM_MOUSEMOVE:
            if (OnDragMouseMove()) return 0;
            return 0;

        case WM_SETCURSOR:
            // The only visible sign that move mode is armed.
            if (moveMode_ || dragging_) {
                SetCursor(LoadCursorW(nullptr,
                                      TaskbarIsHorizontal() ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
            break;

        case WM_CAPTURECHANGED:
            // Something took the mouse away mid-drag — a shell menu, a lock
            // screen. Commit where it currently sits rather than snapping back.
            // OnDragButtonUp clears the flags itself, so it must be called
            // before they are touched.
            OnDragButtonUp();
            return 0;

        case WM_RBUTTONUP:
        case WM_LBUTTONUP: {
            // A release that ends a drag must not also open the menu.
            if (OnDragButtonUp()) return 0;

            POINT cursor{};
            GetCursorPos(&cursor);
            OnRightClick(cursor);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kTaskbarPollTimerId) {
                const TaskbarInfo current = QueryTaskbar();
                // The notification area's bounds are compared too, not just the
                // taskbar's: the widget is anchored to its left edge, and that
                // edge moves whenever an icon appears, the chevron expands, or
                // the clock changes width. Without this the widget would drift
                // out of alignment until something else forced a relayout.
                const bool moved =
                    current.valid &&
                    (current.bounds.left != taskbar_.bounds.left ||
                     current.bounds.top != taskbar_.bounds.top ||
                     current.bounds.right != taskbar_.bounds.right ||
                     current.bounds.bottom != taskbar_.bounds.bottom ||
                     current.notifyBounds.left != taskbar_.notifyBounds.left ||
                     current.notifyBounds.top != taskbar_.notifyBounds.top ||
                     current.hasNotifyArea != taskbar_.hasNotifyArea ||
                     current.dpi != taskbar_.dpi || current.window != taskbar_.window);

                if (moved) {
                    taskbar_ = current;
                    if (current.dpi != dpi_) {
                        dpi_ = current.dpi;
                        EnsureFont();
                    }
                    availableThickness_ = UsableThickness(taskbar_);
                    Relayout();
                }
            }
            return 0;

        case WM_DPICHANGED:
            OnDpiChanged();
            return 0;

        case WM_SETTINGCHANGE:
            // Covers a theme switch, which changes the text colour we read from
            // COLOR_BTNTEXT.
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_DESTROY:
            KillTimer(window, kTaskbarPollTimerId);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

// ------------------------------------------------------------------- drawing

void App::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    if (!dc) return;

    RECT client{};
    GetClientRect(window_, &client);

    // Double buffered: the taskbar is a busy background and painting cell by
    // cell straight to the screen tears visibly.
    ScopedMemoryDC memory(CreateCompatibleDC(dc));
    ScopedBitmap buffer(
        CreateCompatibleBitmap(dc, client.right - client.left, client.bottom - client.top));

    if (memory && buffer) {
        SelectGuard bufferGuard(memory.get(), buffer.get());

        // Clear to the colour key, which the layered window turns transparent.
        //
        // This must be a real fill, not a blit from the window DC: nothing ever
        // erases our background (hbrBackground is null and WM_ERASEBKGND is
        // claimed), so that DC still holds the previous frame and the latency
        // text would smear over itself — "1234 ms" shrinking to "12 ms" would
        // leave the trailing glyphs behind.
        ScopedBrush chroma(CreateSolidBrush(kChromaKey));
        if (chroma) {
            RECT full{0, 0, client.right - client.left, client.bottom - client.top};
            FillRect(memory.get(), &full, chroma.get());
        }

        int x = client.left;
        for (auto& monitor : monitors_) {
            RECT slot = client;
            slot.left = x;
            slot.right = x + monitor->DesiredWidth();

            monitor->Paint(memory.get(), slot, font_.get());

            x = slot.right + MulDiv(kMonitorGap, dpi_, 96);
        }

        BitBlt(dc, 0, 0, client.right - client.left, client.bottom - client.top,
               memory.get(), 0, 0, SRCCOPY);
    }

    EndPaint(window_, &paint);
}

void App::EnsureFont() {
    if (font_ && fontDpi_ == dpi_) return;

    // Segoe UI to match the shell, at 12 pt — deliberately a couple of points
    // larger than the taskbar clock. The clock is read at a glance because you
    // already know roughly what it says; a latency figure is read properly, and
    // at 9 pt next to a 6 px grid it was too small to be useful.
    LOGFONTW logical{};
    logical.lfHeight = -MulDiv(12, dpi_, 72);
    logical.lfWeight = FW_NORMAL;
    logical.lfCharSet = DEFAULT_CHARSET;
    logical.lfOutPrecision = OUT_TT_PRECIS;
    logical.lfQuality = CLEARTYPE_QUALITY;
    logical.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(logical.lfFaceName, L"Segoe UI");

    font_.reset(CreateFontIndirectW(&logical));
    fontDpi_ = dpi_;
}

// -------------------------------------------------------------------- layout

void App::Relayout() {
    EnsureFont();

    int total = 0;
    const int gap = MulDiv(kMonitorGap, dpi_, 96);

    for (size_t i = 0; i < monitors_.size(); ++i) {
        monitors_[i]->Layout(dpi_, availableThickness_);
        if (i > 0) total += gap;
        total += monitors_[i]->DesiredWidth();
    }

    total = std::max(total, 8);

    // Park the widget immediately left of the notification area, recomputed on
    // every layout because the total width changes with the grid shape and the
    // taskbar itself can move or resize.
    //
    // Anchoring right rather than left is deliberate. On Windows 11 the app
    // buttons are centred and shift as windows open and close, and the far left
    // is where the weather and news widget lives — a left-edge offset put the
    // grid straight on top of it. The notification area is the one region that
    // stays where it is.
    lastTotalWidth_ = total;

    // A drag in progress owns the position until the button comes up; letting a
    // ping-driven relayout recompute it mid-drag would make the widget fight
    // the cursor.
    if (!dragging_) {
        offsetAlong_ = ComputeOffsetAlong(total);
    }

    PositionWidget(window_, taskbar_, hostMode_, offsetAlong_, total, availableThickness_);
    InvalidateRect(window_, nullptr, FALSE);
    UpdateTooltip();
}

bool App::TaskbarIsHorizontal() const {
    return taskbar_.edge == TaskbarEdge::Top || taskbar_.edge == TaskbarEdge::Bottom;
}

int App::ComputeOffsetAlong(int widgetWidth) const {
    const bool horizontal = TaskbarIsHorizontal();

    if (!taskbar_.valid) return MulDiv(kLeftEdgeFallback, dpi_, 96);

    // A position the user dragged to wins over the computed one. Stored as a
    // distance from the taskbar's trailing edge, so it holds through a
    // resolution change.
    const WidgetPlacement& placement = store::Document().placement;
    if (placement.manual) {
        const int barLength = horizontal ? (taskbar_.bounds.right - taskbar_.bounds.left)
                                         : (taskbar_.bounds.bottom - taskbar_.bounds.top);
        const int extent = horizontal ? widgetWidth : availableThickness_;
        const int fromEnd = MulDiv(placement.offsetFromRight, dpi_, 96);

        // Clamped so a saved position from a wider screen cannot put the widget
        // off the end of a narrower one.
        return std::clamp(barLength - fromEnd - extent, 0, std::max(0, barLength - extent));
    }

    if (horizontal) {
        if (!taskbar_.hasNotifyArea) {
            // No notification area found: fall back to hugging the right edge
            // of the taskbar, which is still better than the left.
            const int barWidth = taskbar_.bounds.right - taskbar_.bounds.left;
            return std::max(0, barWidth - widgetWidth -
                                   MulDiv(kNotifyAreaGap, dpi_, 96));
        }

        // Taskbar-relative: the notification area's left edge, minus our width
        // and a gap. PositionWidget adds the taskbar origin back when floating.
        const int notifyLeftRelative =
            taskbar_.notifyBounds.left - taskbar_.bounds.left;

        return std::max(0, notifyLeftRelative - widgetWidth -
                               MulDiv(kNotifyAreaGap, dpi_, 96));
    }

    // Docked left or right: the notification area sits at the bottom, so the
    // widget goes above it.
    const int barHeight = taskbar_.bounds.bottom - taskbar_.bounds.top;

    if (!taskbar_.hasNotifyArea) {
        return std::max(0, barHeight - availableThickness_ -
                               MulDiv(kNotifyAreaGap, dpi_, 96));
    }

    const int notifyTopRelative = taskbar_.notifyBounds.top - taskbar_.bounds.top;
    return std::max(0, notifyTopRelative - availableThickness_ -
                           MulDiv(kNotifyAreaGap, dpi_, 96));
}

void App::AttachToTaskbar() {
    taskbar_ = QueryTaskbar();

    if (!taskbar_.valid) {
        hostMode_ = HostMode::Floating;
        return;
    }

    dpi_ = taskbar_.dpi;
    availableThickness_ = UsableThickness(taskbar_);

    hostMode_ = EmbedInTaskbar(window_, taskbar_);
    // Relayout() computes the real position; this only ensures the window is
    // not briefly drawn at the far left before that happens.
    offsetAlong_ = ComputeOffsetAlong(0);

    // Re-apply the colour key after re-parenting. WS_EX_LAYERED survives the
    // style edits in EmbedInTaskbar, but the attributes attached to the layer
    // are not documented to survive a top-level-to-child transition or a change
    // of parent. Setting them again costs nothing and is the difference between
    // a transparent widget and an opaque black rectangle on the taskbar.
    //
    // This also covers Explorer restarts, which come back through here.
    SetLayeredWindowAttributes(window_, kChromaKey, 0, LWA_COLORKEY);

}

void App::OnTaskbarCreated() {
    // Explorer restarted: the old parent window is gone, the notification icon
    // went with it, and both have to be re-established.
    trayIconAdded_ = false;

    AttachToTaskbar();
    Relayout();
    AddTrayIcon();

    ShowWindow(window_, SW_SHOWNOACTIVATE);
}

void App::OnDpiChanged() {
    const TaskbarInfo current = QueryTaskbar();
    if (current.valid) {
        taskbar_ = current;
        dpi_ = current.dpi;
        availableThickness_ = UsableThickness(taskbar_);
    }

    EnsureFont();
    // The swatch cache is keyed by DPI as well as colour, so entries for the
    // old DPI simply stop being looked up. Clearing here would be wrong: a DPI
    // change can arrive while a menu is open, because TrackPopupMenuEx runs its
    // own message pump, and that menu is holding bitmaps from this cache.
    Relayout();
}

// ------------------------------------------------------------------- results

void App::OnResult(unsigned monitorIndex, PingResult* result) {
    if (!result) return;
    if (monitorIndex >= monitors_.size()) return;   // monitor was removed mid-flight

    monitors_[monitorIndex]->HandleResult(*result);
    UpdateTooltip();
}

// --------------------------------------------------------------------- input

MonitorController* App::MonitorAtPoint(POINT clientPoint) {
    int x = 0;
    const int gap = MulDiv(kMonitorGap, dpi_, 96);

    for (auto& monitor : monitors_) {
        const int width = monitor->DesiredWidth();
        // The gap belongs to the monitor on its left, so there is no dead zone
        // between grids where a click does nothing.
        if (clientPoint.x >= x && clientPoint.x < x + width + gap) {
            return monitor.get();
        }
        x += width + gap;
    }

    return monitors_.empty() ? nullptr : monitors_.back().get();
}

void App::OnRightClick(POINT screenPoint) {
    POINT client = screenPoint;
    ScreenToClient(window_, &client);

    if (MonitorController* monitor = MonitorAtPoint(client)) {
        monitor->ShowMenu(screenPoint);
    }
}

// ------------------------------------------------------------------ monitors

void App::DuplicateMonitor(MonitorController* source) {
    if (!source) return;
    if (monitors_.size() >= static_cast<size_t>(defaults::kMaxMonitors)) return;

    MonitorRecord copy;
    // A fresh identity, the same settings.
    GUID guid{};
    if (SUCCEEDED(CoCreateGuid(&guid))) {
        wchar_t buffer[40];
        swprintf(buffer, 40, L"%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                 guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                 guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5],
                 guid.Data4[6], guid.Data4[7]);
        copy.id = buffer;
    } else {
        wchar_t buffer[32];
        swprintf(buffer, 32, L"m%lu", GetTickCount());
        copy.id = buffer;
    }
    copy.settings = source->Settings();

    monitors_.push_back(std::make_unique<MonitorController>(
        this, copy, window_, static_cast<unsigned>(monitors_.size())));

    PersistMonitors();
    Relayout();
}

void App::RemoveMonitor(MonitorController* target) {
    if (!target || monitors_.size() <= 1) return;

    // Deferred on purpose. This is reached from the target's own menu command
    // handler, which is itself called from its ShowMenu — erasing it now would
    // run ~MonitorController while two of its member functions are still on the
    // stack. Posting lets both return first.
    //
    // The id rather than the index: indices are renumbered on every removal, so
    // a queued index could point at a different monitor by the time it is
    // handled. Ids never move.
    pendingRemovalId_ = target->Id();
    PostMessageW(window_, WM_PINGER_REMOVE, 0, 0);
}

void App::RemoveMonitorAt(unsigned) {
    const std::wstring id = pendingRemovalId_;
    pendingRemovalId_.clear();

    if (id.empty() || monitors_.size() <= 1) return;

    bool removed = false;
    for (auto it = monitors_.begin(); it != monitors_.end(); ++it) {
        if ((*it)->Id() != id) continue;
        monitors_.erase(it);   // the destructor stops its ping session
        removed = true;
        break;
    }

    if (!removed) return;

    // Indices are also the wParam the workers post back, so they have to be
    // renumbered as soon as one is removed. SetIndex forwards to the ping
    // session, which is what actually stamps the wParam.
    for (size_t i = 0; i < monitors_.size(); ++i) {
        monitors_[i]->SetIndex(static_cast<unsigned>(i));
    }

    PersistMonitors();
    Relayout();
}

void App::PersistMonitors() {
    std::vector<MonitorRecord> records;
    records.reserve(monitors_.size());
    for (const auto& monitor : monitors_) {
        records.push_back(monitor->Record());
    }
    store::PersistMonitors(records);
}

// ------------------------------------------------------------------ tray icon

bool App::AddTrayIcon() {
    if (trayIconAdded_) return true;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = WM_PINGER_TRAY;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"Pinger");

    trayIconAdded_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    return trayIconAdded_;
}

void App::RemoveTrayIcon() {
    if (!trayIconAdded_) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;

    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

void App::ShowTrayMenu(POINT screenPoint) {
    // The tray icon exists so there is always a way back if the widget ends up
    // somewhere unhelpful, so its menu is the first monitor's menu.
    if (!monitors_.empty()) {
        monitors_.front()->ShowMenu(screenPoint);
    }
}

void App::UpdateTooltip() {
    // Kept simple on purpose: the first monitor's text, refreshed in place.
    // A real multi-region tooltip would need a TTM_ tracking control per grid,
    // which is a lot of machinery for a hover hint.
    if (!trayIconAdded_ || monitors_.empty()) return;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_TIP;

    const std::wstring text = monitors_.front()->TooltipText();
    wcsncpy_s(data.szTip, text.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &data);
}

}  // namespace pinger
