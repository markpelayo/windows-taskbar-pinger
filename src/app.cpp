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
// Retries rebuilding the widget after the shell destroyed it. TaskbarCreated
// normally arrives first and cancels this; the timer exists so that a shell
// which never returns cannot leave the process running with no window, no tray
// icon and therefore no way for the user to quit it.
constexpr UINT kRecoveryTimerId = 2;
constexpr UINT kRecoveryIntervalMs = 3000;
constexpr UINT kTaskbarPollIntervalMs = 2000;
// How much later than that Windows may fire it, so the wakeup can be batched
// with other system activity rather than waking an idle CPU on its own.
constexpr ULONG kTaskbarPollToleranceMs = 2000;

}  // namespace

// ------------------------------------------------------------------ lifetime

App::App() = default;

App::~App() {
    quitting_ = true;
    RemoveTrayIcon();

    // Controllers must die before the window they post results to.
    monitors_.clear();

    // Anything a worker posted before it stopped is still in the queue and owns
    // heap memory nobody will free once the window goes.
    if (window_) {
        MSG queued;
        while (PeekMessageW(&queued, window_, WM_PINGER_RESULT, WM_PINGER_RESULT,
                            PM_REMOVE)) {
            delete reinterpret_cast<PingResult*>(queued.lParam);
        }
    }

    ClearSwatchCache();

    if (menuOwner_) {
        DestroyWindow(menuOwner_);
        menuOwner_ = nullptr;
    }

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

    if (!CreateWidgetWindow()) return false;

    // The hidden top-level window that owns popup menus and dialogs. It exists
    // solely to be something SetForegroundWindow will accept, which a WS_CHILD
    // widget is not. See App::MenuOwner.
    //
    // 1x1 and parked far off-screen rather than 0x0 at the origin: a zero-sized
    // window is a poor candidate for the foreground, and some versions of
    // Windows will not activate one at all — which would defeat the only reason
    // it exists.
    menuOwner_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"Pinger menus",
                                 WS_POPUP, -32000, -32000, 1, 1, nullptr, nullptr,
                                 instance, this);
    if (!menuOwner_) return false;

    // Shown without activating. A window that has never been shown cannot become
    // the foreground window; WS_EX_TOOLWINDOW plus the off-screen position keep
    // it out of Alt-Tab, off the taskbar and out of sight.
    ShowWindow(menuOwner_, SW_SHOWNA);

    // Registered only now that both windows exist. Registering it earlier left
    // a window during CreateWindowExW where a broadcast could be delivered with
    // window_ still null — and Relayout would then call
    // InvalidateRect(nullptr, ...), which repaints every window on the desktop.
    taskbarCreatedMessage_ = RegisterTaskbarCreatedMessage();

    // Read once here and refreshed on WM_SETTINGCHANGE. It used to be fetched
    // from the registry inside every Paint — one RegGetValueW per monitor per
    // second, forever, for a value that only changes on a theme switch.
    taskbarTextColor_ = TaskbarTextColor();

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
    Relayout();

    ShowWindow(window_, SW_SHOWNOACTIVATE);
    AddTrayIcon();

    StartTaskbarPoll();
    return true;
}

void App::StartTaskbarPoll() {
    if (!window_) return;

    // A coalescable timer lets Windows batch this wakeup with whatever else the
    // system is already doing, instead of forcing the CPU out of idle on its own
    // schedule twice a second. That is the difference that shows up in laptop
    // battery life; the CPU cost of the poll itself is microseconds.
    //
    // Windows 8 and later, resolved dynamically because the manifest still
    // declares 8.1 support and this must not become a hard dependency.
    using SetCoalescableTimerFn =
        UINT_PTR(WINAPI*)(HWND, UINT_PTR, UINT, TIMERPROC, ULONG);

    static SetCoalescableTimerFn coalescable = []() -> SetCoalescableTimerFn {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return nullptr;
        return reinterpret_cast<SetCoalescableTimerFn>(
            GetProcAddress(user32, "SetCoalescableTimer"));
    }();

    if (coalescable) {
        coalescable(window_, kTaskbarPollTimerId, kTaskbarPollIntervalMs, nullptr,
                    kTaskbarPollToleranceMs);
    } else {
        SetTimer(window_, kTaskbarPollTimerId, kTaskbarPollIntervalMs, nullptr);
    }
}

bool App::CreateWidgetWindow() {
    // Created as a popup first, then converted to a child if embedding works.
    // WS_EX_LAYERED gives us the colour key that lets the taskbar show through;
    // without it the widget would sit on an opaque rectangle, and GDI cannot
    // reproduce the Windows 11 acrylic behind it.
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
                              kWindowClass, kWindowTitle, WS_POPUP, 0, 0, 100, 24,
                              nullptr, nullptr, instance_, this);

    if (!window_) return false;

    SetLayeredWindowAttributes(window_, kChromaKey, 0, LWA_COLORKEY);

    // The old window's buffer belonged to the old DC.
    buffer_.reset();
    bufferDc_.reset();
    bufferWidth_ = 0;
    bufferHeight_ = 0;

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

void App::RequestQuit() {
    quitting_ = true;
    PostQuitMessage(0);
}

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

bool App::OnDragButtonUp(bool releaseCapture) {
    if (!dragging_) return false;

    // Flags first. MSDN forbids calling ReleaseCapture while handling
    // WM_CAPTURECHANGED, and clearing these before the call also stops a
    // re-entrant notification from running this function twice and writing the
    // position to disk twice.
    dragging_ = false;
    moveMode_ = false;   // one drag per arming

    if (releaseCapture) ReleaseCapture();

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

// --------------------------------------------------- owner-drawn profile rows
//
// The macOS version put an ✕ button on each saved-profile row using a custom
// NSView. Win32 menu items cannot host child controls, so the row is drawn by
// hand instead: the name on the left, a dismiss glyph on the right, and a hit
// test on the way out that decides which of the two the click meant.
//
// The hit test has to be done indirectly. TrackPopupMenuEx returns only a
// command id, and by then the menu window is destroyed, so the item's on-screen
// rectangle is captured while it is still highlighted — see OnMenuSelect.

namespace {

// Width of the glyph column, in logical pixels at 96 DPI.
constexpr int kDeleteGlyphColumn = 26;

// Left inset for the row's text, chosen to line up with the check-mark gutter
// of the ordinary items above and below it.
constexpr int kProfileTextInset = 22;

// U+2715 MULTIPLICATION X. Segoe UI has it, and it reads as a dismiss control
// rather than as the letter x.
constexpr wchar_t kDeleteGlyph[] = L"✕";

}  // namespace

void App::SetProfileMenuNames(std::vector<std::wstring> names) {
    profileMenuNames_ = std::move(names);
}

HFONT App::MenuFont() {
    // Rebuilt when the DPI changes, otherwise the owner-drawn profile rows would
    // stay at the old point size while every ordinary item around them scaled.
    if (menuFont_ && menuFontDpi_ == dpi_) return menuFont_.get();
    menuFont_.reset();

    // The shell's own menu font, so an owner-drawn row is indistinguishable
    // from the ordinary items around it.
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        menuFont_.reset(CreateFontIndirectW(&metrics.lfMenuFont));
        menuFontDpi_ = dpi_;
    }

    return menuFont_.get();
}

void App::OnMeasureProfileItem(MEASUREITEMSTRUCT* measure) {
    if (!measure || measure->CtlType != ODT_MENU) return;

    // Item data is the index plus one; see the AppendMenuW call that sets it.
    if (measure->itemData == 0) return;
    const size_t index = static_cast<size_t>(measure->itemData) - 1;
    if (index >= profileMenuNames_.size()) return;

    const std::wstring& name = profileMenuNames_[index];

    ScopedWindowDC screen(nullptr, GetDC(nullptr));
    if (!screen) return;

    HFONT font = MenuFont();
    if (!font) return;

    SelectGuard fontGuard(screen.get(), font);

    SIZE extent{};
    GetTextExtentPoint32W(screen.get(), name.c_str(), static_cast<int>(name.size()),
                          &extent);

    measure->itemWidth = static_cast<UINT>(extent.cx + MulDiv(
        kProfileTextInset + kDeleteGlyphColumn + 8, dpi_, 96));
    // A couple of pixels of breathing room, and never shorter than a standard
    // menu row or the list looks cramped next to the items around it.
    measure->itemHeight = static_cast<UINT>(
        std::max<int>(extent.cy + MulDiv(6, dpi_, 96),
                      GetSystemMetrics(SM_CYMENU)));
}

void App::OnDrawProfileItem(DRAWITEMSTRUCT* draw) {
    if (!draw || draw->CtlType != ODT_MENU || !draw->hDC) return;

    if (draw->itemData == 0) return;
    const size_t index = static_cast<size_t>(draw->itemData) - 1;
    if (index >= profileMenuNames_.size()) return;

    const std::wstring& name = profileMenuNames_[index];
    const bool selected = (draw->itemState & ODS_SELECTED) != 0;

    // Background first. GetSysColorBrush returns a shared brush that must not
    // be deleted, which is why it is used directly rather than wrapped.
    FillRect(draw->hDC, &draw->rcItem,
             GetSysColorBrush(selected ? COLOR_HIGHLIGHT : COLOR_MENU));

    HFONT font = MenuFont();
    if (!font) return;

    SelectGuard fontGuard(draw->hDC, font);
    const int previousMode = SetBkMode(draw->hDC, TRANSPARENT);
    const COLORREF previousColor = SetTextColor(
        draw->hDC, GetSysColor(selected ? COLOR_HIGHLIGHTTEXT : COLOR_MENUTEXT));

    const int glyphColumn = MulDiv(kDeleteGlyphColumn, dpi_, 96);

    RECT textRect = draw->rcItem;
    textRect.left += MulDiv(kProfileTextInset, dpi_, 96);
    textRect.right -= glyphColumn;
    DrawTextW(draw->hDC, name.c_str(), static_cast<int>(name.size()), &textRect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // The glyph is drawn dimmer than the name unless the row is highlighted,
    // so it reads as a secondary action rather than competing with the label.
    if (!selected) {
        SetTextColor(draw->hDC, GetSysColor(COLOR_GRAYTEXT));
    }

    RECT glyphRect = draw->rcItem;
    glyphRect.left = glyphRect.right - glyphColumn;
    DrawTextW(draw->hDC, kDeleteGlyph, -1, &glyphRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(draw->hDC, previousColor);
    SetBkMode(draw->hDC, previousMode);
}

void App::ClearMenuSelection() {
    lastMenuItemValid_ = false;
}

void App::OnMenuSelect(WPARAM wParam, LPARAM lParam) {
    HMENU menu = reinterpret_cast<HMENU>(lParam);
    const UINT flags = HIWORD(wParam);

    // Windows sends a final WM_MENUSELECT with flags 0xFFFF and a null menu
    // when the menu closes — on selection as well as on Escape — and it arrives
    // *before* TrackPopupMenuEx returns the command. Clearing the captured rect
    // here would therefore destroy it a moment before the command handler needs
    // it. So this notification is ignored entirely; ShowMenu clears the state up
    // front instead, which also stops a rect from one menu leaking into the next.
    if (flags == 0xFFFF && menu == nullptr) return;

    lastMenuItemValid_ = false;

    // Submenu headings, separators and the informational rows all report a
    // command of 0, and none of them has a row worth hit-testing. Capturing one
    // would leave a rectangle behind that belongs to the wrong item.
    if (!menu || (flags & MF_POPUP) || LOWORD(wParam) == 0) return;

    // GetMenuItemRect takes a *position*, not a command id — there is no
    // MF_BYCOMMAND form of it — while WM_MENUSELECT reports the command id.
    // Passing the id straight through made every call fail, which is what left
    // the delete glyph drawn but inert.
    const UINT command = LOWORD(wParam);
    const int count = GetMenuItemCount(menu);
    int position = -1;

    for (int i = 0; i < count; ++i) {
        if (GetMenuItemID(menu, i) == command) {
            position = i;
            break;
        }
    }

    if (position < 0) return;

    // Screen rectangle of the row under the cursor, captured now because the
    // menu window is destroyed before TrackPopupMenuEx returns.
    RECT rect{};
    if (GetMenuItemRect(menuOwner_, menu, static_cast<UINT>(position), &rect)) {
        lastMenuItemRect_ = rect;
        lastMenuItemValid_ = true;
    }
}

bool App::LastSelectionHitDeleteGlyph() const {
    if (!lastMenuItemValid_) return false;

    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;

    // Keyboard selection leaves the cursor wherever it happened to be, so a
    // click outside the row is treated as "not the glyph" — the safe reading,
    // since the destructive action should never be the accidental one.
    if (cursor.y < lastMenuItemRect_.top || cursor.y > lastMenuItemRect_.bottom) {
        return false;
    }

    const int glyphColumn = MulDiv(kDeleteGlyphColumn, dpi_, 96);
    return cursor.x >= (lastMenuItemRect_.right - glyphColumn) &&
           cursor.x <= lastMenuItemRect_.right;
}

// -------------------------------------------------------------------- window

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wParam,
                                 LPARAM lParam) {
    App* app = nullptr;

    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = static_cast<App*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        // Two windows share this proc — the widget and the hidden menu owner —
        // so the handles are assigned by Initialise after each call returns,
        // not here where we cannot tell them apart.
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
        // Broadcast to every top-level window, and in floating mode the widget
        // is top-level too — so without this guard it runs twice, and the
        // second pass leaves trayIconAdded_ false, orphaning the tray icon on
        // exit. The owner is always top-level, so it is the reliable listener;
        // an embedded widget is a WS_CHILD and gets no broadcasts at all.
        if (window == menuOwner_) OnTaskbarCreated();
        return 0;
    }

    switch (message) {
        case WM_PAINT:
            // The owner is never visible, so it should never be painted — and
            // returning without validating its update region would spin.
            if (window != window_) break;
            OnPaint();
            return 0;

        case WM_ERASEBKGND:
            if (window != window_) break;
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
            if (window != window_) break;
            if (OnDragButtonDown()) return 0;
            return 0;

        case WM_MOUSEMOVE:
            if (window != window_) break;
            if (OnDragMouseMove()) return 0;
            return 0;

        case WM_SETCURSOR:
            if (window != window_) break;
            // The only visible sign that move mode is armed.
            if (moveMode_ || dragging_) {
                SetCursor(LoadCursorW(nullptr,
                                      TaskbarIsHorizontal() ? IDC_SIZEWE : IDC_SIZENS));
                return TRUE;
            }
            break;

        case WM_CAPTURECHANGED:
            if (window != window_) break;
            // Something took the mouse away mid-drag — a shell menu, a lock
            // screen. Commit where it currently sits rather than snapping back.
            // Capture is already gone, so do not release it again.
            OnDragButtonUp(false);
            return 0;

        case WM_RBUTTONUP:
        case WM_LBUTTONUP: {
            // The owner is the foreground window while a menu is up; letting a
            // button-up there reach OnRightClick would reopen the menu.
            if (window != window_) break;

            // A release that ends a drag must not also open the menu.
            if (OnDragButtonUp(true)) return 0;

            POINT cursor{};
            GetCursorPos(&cursor);
            OnRightClick(cursor);
            return 0;
        }

        case WM_TIMER:
            if (wParam == kRecoveryTimerId) {
                // The shell did not send TaskbarCreated, or sent it before we
                // were ready. Try to rebuild anyway.
                if (!window_) {
                    OnTaskbarCreated();
                } else {
                    KillTimer(menuOwner_, kRecoveryTimerId);
                }
                return 0;
            }

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
                    }
                    availableThickness_ = UsableThickness(taskbar_);
                    Relayout();
                }
            }
            return 0;

        case WM_DPICHANGED:
            // Guarded like the other cases: menuOwner_ is top-level and would
            // otherwise run this a second time, including while window_ is null
            // during an Explorer restart.
            if (window != window_) break;
            OnDpiChanged();
            return 0;

        case WM_SETTINGCHANGE: {
            // Broadcast whenever *any* process calls SystemParametersInfo, so it
            // fires for mouse settings, power settings, policy refreshes and
            // much else. Only a theme change matters here, and that arrives as
            // "ImmersiveColorSet".
            const wchar_t* area = reinterpret_cast<const wchar_t*>(lParam);
            const bool themeChanged =
                area == nullptr || wcscmp(area, L"ImmersiveColorSet") == 0;

            if (themeChanged) {
                taskbarTextColor_ = TaskbarTextColor();
                // Menu metrics can change with the theme too.
                menuFont_.reset();
                menuFontDpi_ = 0;
                // This is broadcast to top-level windows only, so once embedded
                // it arrives at the menu owner — repaint the widget, not
                // whichever window happened to be notified.
                if (window_) InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MEASUREITEM:
            OnMeasureProfileItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam));
            return TRUE;

        case WM_DRAWITEM:
            OnDrawProfileItem(reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;

        case WM_MENUSELECT:
            OnMenuSelect(wParam, lParam);
            return 0;

        case WM_DESTROY:
            // The hidden menu owner shares this proc, so only the widget's own
            // destruction is interesting.
            if (window == window_) {
                KillTimer(window, kTaskbarPollTimerId);

                if (quitting_) {
                    PostQuitMessage(0);
                } else {
                    // The shell destroyed us, not the user. Once embedded, the
                    // widget is a WS_CHILD of Shell_TrayWnd, so an Explorer
                    // restart takes it down with the taskbar — and quitting here
                    // meant the app simply vanished whenever Explorer restarted,
                    // with the TaskbarCreated handling below never getting a
                    // chance to run.
                    //
                    // Anything the workers posted to this window is about to be
                    // discarded with it, and those payloads are heap-owned.
                    MSG queued;
                    while (PeekMessageW(&queued, window, WM_PINGER_RESULT,
                                        WM_PINGER_RESULT, PM_REMOVE)) {
                        delete reinterpret_cast<PingResult*>(queued.lParam);
                    }

                    window_ = nullptr;
                    trayIconAdded_ = false;

                    // Rebuild on the broadcast, or failing that on this timer.
                    // Without the fallback, a shell that never restarts would
                    // leave the process alive with nothing on screen and no tray
                    // icon — unquittable except through Task Manager.
                    SetTimer(menuOwner_, kRecoveryTimerId, kRecoveryIntervalMs,
                             nullptr);
                }
            }
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

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    if (width > 0 && height > 0) {
        // The back buffer is rebuilt only when the widget changes size, not on
        // every frame. Double buffering is needed because the taskbar is a busy
        // background and painting cell by cell straight to the screen tears —
        // but creating the DC and bitmap each time meant two GDI allocations a
        // second, forever, which made the handle count oscillate and looked
        // exactly like a leak to anyone watching Task Manager.
        if (!bufferDc_) bufferDc_.reset(CreateCompatibleDC(dc));

        if (bufferDc_ && (!buffer_ || width != bufferWidth_ || height != bufferHeight_)) {
            buffer_.reset(CreateCompatibleBitmap(dc, width, height));
            bufferWidth_ = width;
            bufferHeight_ = height;
        }

        // The colour key never changes, so neither does its brush.
        if (!chromaBrush_) chromaBrush_.reset(CreateSolidBrush(kChromaKey));

        if (bufferDc_ && buffer_ && chromaBrush_) {
            SelectGuard bufferGuard(bufferDc_.get(), buffer_.get());

            // Clear to the colour key, which the layered window turns
            // transparent. This must be a real fill rather than a blit from the
            // window DC: nothing erases our background, so that DC still holds
            // the previous frame and the latency text would smear over itself.
            RECT full{0, 0, width, height};
            FillRect(bufferDc_.get(), &full, chromaBrush_.get());

            int x = client.left;
            for (auto& monitor : monitors_) {
                RECT slot = client;
                slot.left = x;
                slot.right = x + monitor->DesiredWidth();

                monitor->Paint(bufferDc_.get(), slot, taskbarTextColor_);

                x = slot.right + MulDiv(kMonitorGap, dpi_, 96);
            }

            BitBlt(dc, 0, 0, width, height, bufferDc_.get(), 0, 0, SRCCOPY);
        }
    }

    EndPaint(window_, &paint);
}

// -------------------------------------------------------------------- layout

void App::Relayout() {

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

    // Guarded: window_ is briefly null between the shell destroying the widget
    // and TaskbarCreated rebuilding it, and InvalidateRect(nullptr, ...) means
    // "repaint every window on the desktop".
    if (window_) InvalidateRect(window_, nullptr, FALSE);
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
    // Explorer restarted: the old parent is gone, the notification icon went
    // with it, and — because the widget was a child of the taskbar — so did the
    // widget window itself.
    trayIconAdded_ = false;

    if (!window_) {
        if (!CreateWidgetWindow()) return;

        KillTimer(menuOwner_, kRecoveryTimerId);

        // Every monitor and every ping session captured the old handle, so they
        // all have to be pointed at the replacement.
        for (auto& monitor : monitors_) {
            monitor->RebindWindow(window_);
        }

        StartTaskbarPoll();
    }

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

    // Fonts are rebuilt by each monitor's Layout, which Relayout calls.
    //
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
    // TrackPopupMenuEx runs its own message pump, so this posted message can be
    // dispatched while a ShowMenu is still on the stack — destroying the very
    // controller whose menu handler asked for the removal. Re-post and wait.
    if (MenuIsOpen()) {
        PostMessageW(window_, WM_PINGER_REMOVE, 0, 0);
        return;
    }

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
    // Rate limited, because Shell_NotifyIconW is a cross-process call into
    // Explorer and this text is only ever read while the pointer is resting on
    // the tray icon. It used to run on every settled packet — once a second,
    // forever, waking another process to update something nobody was looking at.
    const ULONGLONG now = GetTickCount64();
    if (lastTooltipUpdate_ != 0 && now - lastTooltipUpdate_ < 5000) return;
    lastTooltipUpdate_ = now;

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
