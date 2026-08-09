// monitor.cpp — one grid's state, menu and per-packet logic.

#include "monitor.h"

#include <shellapi.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include "app.h"
#include "dialogs.h"
#include "resource.h"
#include "taskbar.h"

namespace pinger {

namespace {

// Gap between the grid and the latency readout, in logical pixels at 96 DPI.
constexpr int kLatencyTextGap = 6;

// A couple of pixels past the measured text, so the last glyph is not flush
// against the notification area.
constexpr int kLatencyTextPadding = 3;

void AppendItem(HMENU menu, UINT flags, UINT_PTR id, const wchar_t* text) {
    AppendMenuW(menu, flags, id, text);
}

void AppendSeparator(HMENU menu) {
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
}

// Attaches a colour swatch to a menu item, so the colour menus show what they
// mean rather than only naming it.
void SetItemBitmap(HMENU menu, UINT id, HBITMAP bitmap) {
    if (!bitmap) return;

    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_BITMAP;
    info.hbmpItem = bitmap;
    SetMenuItemInfoW(menu, id, FALSE, &info);
}

}  // namespace

// ------------------------------------------------------------------ lifetime

MonitorController::MonitorController(App* app, const MonitorRecord& record, HWND host,
                                     unsigned index)
    : app_(app), record_(record), window_(host), index_(index) {
    record_.settings.Sanitise();

    session_ = std::make_unique<PingSession>(host, index);
    Restart();
}

MonitorController::~MonitorController() {
    // Stop the worker before anything it posts to could go away.
    if (session_) session_->Stop();
    renderer_.Reset();
}

void MonitorController::SetIndex(unsigned index) {
    index_ = index;
    if (session_) session_->SetMonitorIndex(index);
}

void MonitorController::RebindWindow(HWND window) {
    window_ = window;

    // The session captured the old handle, so it cannot simply be told the new
    // one — it is replaced outright. Restart() re-arms the opening-packet rule
    // as usual, which is correct here: this is a fresh session on a fresh window.
    session_ = std::make_unique<PingSession>(window_, index_);
    Restart();
}

void MonitorController::Restart() {
    // A new session sends its opening packet immediately, and opening packets
    // are routinely lost to route setup — ARP, DNS, a Wi-Fi radio waking up.
    // The hold-back rule has to be re-armed for every session, not just the
    // first, or changing the ping frequency would paint a red cell each time.
    // (The macOS original does this in beginSession() for the same reason.)
    firstOutcomeSettled_ = false;
    holdingFirstFailure_ = false;

    if (session_) session_->Start(record_.settings.host, record_.settings.interval);
}

// -------------------------------------------------------------------- layout

int MonitorController::DesiredWidth() const {
    int width = metrics_.width;
    if (record_.settings.showLatency) {
        width += MulDiv(kLatencyTextGap, dpi_, 96) + latencyWidth_;
    }
    return std::max(1, width);
}

// The readout is measured rather than given a fixed allowance.
//
// A fixed worst-case width — wide enough for "9999 ms" — left a large dead gap
// between a short reading like "5 ms" and the notification area, because the
// widget's right edge is where the reserved space ends, not where the text
// does.
//
// Measuring the live string exactly would be worse: the widget is anchored by
// its right edge, so every change in text width shifts the grid sideways, and
// with a per-second update that is a visible twitch.
//
// So the measurement uses a template of the same length with every digit
// replaced by '8', the widest figure. The width then depends only on the
// *number of characters*, which changes when a reading crosses 9 to 10 or 99
// to 100 and not otherwise. Stable in normal use, tight against the tray, and
// it still fits when the number grows.
void MonitorController::EnsureFont() {
    const int size = record_.settings.textSize;
    if (font_ && fontDpi_ == dpi_ && fontSize_ == size) return;

    // Segoe UI to match the shell. The taskbar clock is 9 pt; the default here
    // is 10, and the menu offers 8 to 16.
    LOGFONTW logical{};
    logical.lfHeight = -MulDiv(size, dpi_, 72);
    logical.lfWeight = FW_NORMAL;
    logical.lfCharSet = DEFAULT_CHARSET;
    logical.lfOutPrecision = OUT_TT_PRECIS;
    logical.lfQuality = CLEARTYPE_QUALITY;
    logical.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
    wcscpy_s(logical.lfFaceName, L"Segoe UI");

    font_.reset(CreateFontIndirectW(&logical));
    fontDpi_ = dpi_;
    fontSize_ = size;
}

void MonitorController::MeasureLatencyWidth() {
    EnsureFont();
    if (!font_) return;

    const std::wstring text = AverageLatencyText();

    std::wstring templateText = text;
    for (wchar_t& c : templateText) {
        if (iswdigit(c)) c = L'8';
    }

    // Recorded even when measuring fails below. Otherwise a persistent GDI
    // failure leaves latencyLength_ stale while the text keeps growing, and the
    // grow branch in CommitSample would then request a relayout every sample.
    latencyLength_ = text.size();

    ScopedWindowDC screen(nullptr, GetDC(nullptr));
    if (!screen) return;

    SelectGuard fontGuard(screen.get(), font_.get());

    SIZE extent{};
    if (!GetTextExtentPoint32W(screen.get(), templateText.c_str(),
                               static_cast<int>(templateText.size()), &extent)) {
        return;
    }

    latencyWidth_ = extent.cx + MulDiv(kLatencyTextPadding, dpi_, 96);
}

void MonitorController::Layout(int dpi, int availableThickness) {
    dpi_ = dpi > 0 ? dpi : 96;
    availableThickness_ = availableThickness > 0 ? availableThickness
                                                 : defaults::kMaxBarHeight;
    metrics_ = ComputeMetrics(record_.settings, dpi_, availableThickness_);
    MeasureLatencyWidth();
}

void MonitorController::Paint(HDC dc, const RECT& slot, COLORREF textColor) {
    const std::wstring latency =
        record_.settings.showLatency ? AverageLatencyText() : std::wstring();

    // The colour is passed in rather than fetched here. It comes from
    // TaskbarTextColor, which reads a registry value — fine once, but this runs
    // once a second per monitor, so the app caches it and refreshes it on the
    // theme-change broadcast instead.
    renderer_.Paint(dc, slot, record_.settings, metrics_, samples_, latency, textColor,
                    font_.get(), dpi_);
}

void MonitorController::Invalidate() {
    if (window_) InvalidateRect(window_, nullptr, FALSE);
}

void MonitorController::PersistSettings() {
    SettingsDocument& document = store::Document();
    for (MonitorRecord& saved : document.monitors) {
        if (saved.id == record_.id) {
            saved.settings = record_.settings;
            store::Save();
            return;
        }
    }
}

// ------------------------------------------------------------------ averages

double MonitorController::AverageLatency() const {
    return latencyCount_ > 0 ? latencySum_ / static_cast<double>(latencyCount_) : 0.0;
}

std::wstring MonitorController::AverageLatencyText() const {
    if (latencyCount_ <= 0) return L"— ms";   // em dash

    const double average = AverageLatency();
    if (average < 1.0) return L"<1 ms";

    wchar_t buffer[32];
    swprintf(buffer, 32, L"%.0f ms", average);
    return buffer;
}

std::wstring MonitorController::TooltipText() const {
    std::wstring text = record_.settings.host;

    if (samples_.empty()) {
        text += L": waiting for first ping…";
        return text;
    }

    const Sample& last = samples_.back();
    if (last.reachable) {
        if (last.hasLatency) {
            wchar_t buffer[32];
            swprintf(buffer, 32, L": %.0f ms", last.milliseconds);
            text += buffer;
        } else {
            text += L": reachable";
        }
    } else {
        text += L": unreachable";
    }

    int up = 0;
    for (const Sample& sample : samples_) {
        if (sample.reachable) ++up;
    }
    const int percent =
        static_cast<int>((static_cast<double>(up) / static_cast<double>(samples_.size())) * 100.0 + 0.5);

    wchar_t stats[96];
    swprintf(stats, 96, L"\n%d%% reachable (%d/%d) · avg %s", percent, up,
             static_cast<int>(samples_.size()), AverageLatencyText().c_str());
    text += stats;

    return text;
}

// --------------------------------------------------------------- ping results

void MonitorController::HandleResult(const PingResult& result) {
    // A result from a session we have already replaced is stale: the host or the
    // frequency changed while this packet was in flight.
    if (!session_ || result.session != session_->CurrentSession()) return;

    if (!firstOutcomeSettled_) {
        if (result.reachable) {
            // The held loss was route setup after all; drop it.
            holdingFirstFailure_ = false;
            firstOutcomeSettled_ = true;
        } else if (holdingFirstFailure_) {
            // Two in a row: genuinely down, so draw both.
            firstOutcomeSettled_ = true;
            holdingFirstFailure_ = false;
            CommitSample(heldFirstFailure_);
        } else {
            heldFirstFailure_ = result;
            holdingFirstFailure_ = true;
            return;   // nothing drawn yet
        }
    }

    CommitSample(result);
}

void MonitorController::CommitSample(const PingResult& result) {
    Sample sample;
    sample.reachable = result.reachable;
    sample.hasLatency = result.reachable && result.hasLatency;
    sample.milliseconds = sample.hasLatency ? result.milliseconds : 0.0;

    samples_.push_back(sample);
    if (sample.hasLatency) {
        latencySum_ += sample.milliseconds;
        ++latencyCount_;
    }

    TrimSamples();

    // A reading that has grown a digit needs more room, so the widget has to be
    // re-measured and repositioned. Compared by length rather than value:
    // 5 ms to 6 ms is free, 9 ms to 10 ms is not.
    //
    // Growing is applied at once; shrinking only after the shorter reading has
    // held for a while. Without that hysteresis an average hovering at 9.5 ms —
    // an entirely ordinary gateway latency — flips between "9 ms" and "10 ms"
    // and triggers a full relayout, including a SetWindowPos on a child of the
    // taskbar, potentially every single second.
    if (record_.settings.showLatency) {
        const size_t length = AverageLatencyText().size();

        if (length > latencyLength_) {
            shrinkCandidates_ = 0;
            if (app_) {
                app_->Relayout();   // re-measures, resizes and repaints
                return;
            }
        } else if (length < latencyLength_) {
            if (++shrinkCandidates_ >= kShrinkHoldSamples) {
                shrinkCandidates_ = 0;
                if (app_) {
                    app_->Relayout();
                    return;
                }
            }
        } else {
            shrinkCandidates_ = 0;
        }
    }

    // Every packet moves at least one cell, so unlike the macOS version there
    // is nothing to gain from comparing against the last drawn state — the grid
    // has always changed by the time we get here.
    Invalidate();
}

void MonitorController::TrimSamples() {
    const int capacity = record_.settings.CellCount();
    const int excess = static_cast<int>(samples_.size()) - capacity;
    if (excess <= 0) return;

    // Discount whatever rolls off, so the average always covers exactly the
    // cells currently on screen.
    for (int i = 0; i < excess; ++i) {
        if (samples_[static_cast<size_t>(i)].hasLatency) {
            latencySum_ -= samples_[static_cast<size_t>(i)].milliseconds;
            --latencyCount_;
        }
    }

    samples_.erase(samples_.begin(), samples_.begin() + excess);

    if (latencyCount_ <= 0) {   // guard accumulated float drift
        latencyCount_ = 0;
        latencySum_ = 0.0;
    }
}

void MonitorController::ClearHistory() {
    samples_.clear();
    latencySum_ = 0.0;
    latencyCount_ = 0;
    firstOutcomeSettled_ = false;
    holdingFirstFailure_ = false;
    Invalidate();
}

// ---------------------------------------------------------------------- menu

HMENU MonitorController::BuildMenu(std::vector<HMENU>* ownedSubmenus) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return nullptr;

    const MonitorSettings& s = record_.settings;

    // About
    wchar_t about[160];
    swprintf(about, 160, L"%s %s · by %s", defaults::kProjectName,
             defaults::kVersion, defaults::kAuthor);
    AppendItem(menu, MF_STRING, IDM_ABOUT, about);
    AppendSeparator(menu);

    // Target and rolling stats, both informational.
    std::wstring target = L"Target: " + s.host;
    AppendItem(menu, MF_STRING | MF_GRAYED, 0, target.c_str());

    if (samples_.empty()) {
        AppendItem(menu, MF_STRING | MF_GRAYED, 0, L"Waiting for first ping…");
    } else {
        int up = 0;
        for (const Sample& sample : samples_) {
            if (sample.reachable) ++up;
        }
        const int percent =
            static_cast<int>((static_cast<double>(up) / static_cast<double>(samples_.size())) * 100.0 + 0.5);

        wchar_t stats[128];
        swprintf(stats, 128, L"%d%% reachable (%d/%d) · avg %s", percent, up,
                 static_cast<int>(samples_.size()), AverageLatencyText().c_str());
        AppendItem(menu, MF_STRING | MF_GRAYED, 0, stats);
    }
    AppendSeparator(menu);

    AppendItem(menu, MF_STRING, IDM_SET_HOST, L"Set IP Address or Hostname…");

    // Ping frequency
    HMENU intervals = CreatePopupMenu();
    ownedSubmenus->push_back(intervals);
    const auto& intervalChoices = defaults::IntervalChoices();
    bool intervalIsPreset = false;
    for (size_t i = 0; i < intervalChoices.size(); ++i) {
        const bool checked = fabs(intervalChoices[i] - s.interval) < 0.001;
        if (checked) intervalIsPreset = true;
        AppendItem(intervals, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED),
                   IDM_INTERVAL_FIRST + i, FormatInterval(intervalChoices[i]).c_str());
    }
    AppendSeparator(intervals);
    AppendItem(intervals, MF_STRING | (intervalIsPreset ? MF_UNCHECKED : MF_CHECKED),
               IDM_INTERVAL_CUSTOM, L"Custom…");

    std::wstring intervalTitle = L"Ping frequency: every " + FormatInterval(s.interval);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(intervals),
                intervalTitle.c_str());

    AppendItem(menu, MF_STRING | (s.showLatency ? MF_CHECKED : MF_UNCHECKED),
               IDM_TOGGLE_LATENCY, L"Show average latency");

    // Directly beneath the toggle it belongs to, and greyed out when the
    // readout is hidden — there is nothing for it to size then.
    HMENU textSizes = CreatePopupMenu();
    ownedSubmenus->push_back(textSizes);

    const auto& sizeChoices = defaults::TextSizeChoices();
    for (size_t i = 0; i < sizeChoices.size(); ++i) {
        wchar_t label[32];
        swprintf(label, 32, L"%d pt%s", sizeChoices[i],
                 sizeChoices[i] == defaults::kTextSize ? L"  (default)" : L"");
        AppendItem(textSizes,
                   MF_STRING | (sizeChoices[i] == s.textSize ? MF_CHECKED : MF_UNCHECKED),
                   IDM_TEXTSIZE_FIRST + i, label);
    }

    wchar_t textSizeTitle[64];
    swprintf(textSizeTitle, 64, L"Latency text size: %d pt", s.textSize);
    AppendMenuW(menu, MF_POPUP | (s.showLatency ? MF_ENABLED : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(textSizes), textSizeTitle);

    // Colours
    for (int slotIndex = 0; slotIndex < 2; ++slotIndex) {
        const ColorSlot slot = slotIndex == 0 ? ColorSlot::Success : ColorSlot::Failure;
        const COLORREF current = s.ColorFor(slot);
        const UINT firstId = slot == ColorSlot::Success ? IDM_SUCCESS_FIRST
                                                        : IDM_FAILURE_FIRST;
        const UINT customId = slot == ColorSlot::Success ? IDM_SUCCESS_CUSTOM
                                                         : IDM_FAILURE_CUSTOM;

        HMENU colors = CreatePopupMenu();
        ownedSubmenus->push_back(colors);

        const auto& presets = ColorPresets();
        bool isPreset = false;
        for (size_t i = 0; i < presets.size(); ++i) {
            COLORREF presetColor = 0;
            ColorFromHex(presets[i].hex, &presetColor);
            const bool checked = presetColor == current;
            if (checked) isPreset = true;

            const UINT id = static_cast<UINT>(firstId + i);
            AppendItem(colors, MF_STRING | (checked ? MF_CHECKED : MF_UNCHECKED), id,
                       presets[i].name);
            SetItemBitmap(colors, id, SwatchForColor(presetColor, dpi_));
        }

        AppendSeparator(colors);
        AppendItem(colors, MF_STRING | (isPreset ? MF_UNCHECKED : MF_CHECKED), customId,
                   L"Custom…");

        const UINT_PTR handle = reinterpret_cast<UINT_PTR>(colors);
        AppendMenuW(menu, MF_POPUP, handle, ColorSlotLabel(slot));
        // The swatch on the parent row shows the current colour at a glance.
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_BITMAP;
        info.hbmpItem = SwatchForColor(current, dpi_);
        SetMenuItemInfoW(menu, static_cast<UINT>(GetMenuItemCount(menu) - 1), TRUE,
                         &info);
    }

    // Fill direction, directly beneath the two colour rows it affects.
    //
    // Named for the direction it selects rather than for the setting behind it,
    // and the arrow spells the direction out — "horizontal" alone would not say
    // which corner it starts from, which is the part that actually matters.
    // Unchecked is the default order, described in the menu reference.
    AppendItem(menu, MF_STRING | (s.fillHorizontal ? MF_CHECKED : MF_UNCHECKED),
               IDM_TOGGLE_FILL, L"Fill in rows (top-left → bottom-right)");

    AppendSeparator(menu);

    // Grid shape. Four near-identical numeric submenus.
    struct NumberMenu {
        const wchar_t*           label;
        const std::vector<int>&  choices;
        int                      current;
        UINT                     firstId;
        const wchar_t*           suffix;
    };

    const NumberMenu numberMenus[] = {
        {L"Rows", defaults::RowChoices(), s.rows, IDM_ROWS_FIRST, L""},
        {L"Columns", defaults::ColumnChoices(), s.columns, IDM_COLUMNS_FIRST, L""},
        {L"Cell size", defaults::CellChoices(), s.cell, IDM_CELL_FIRST, L" px"},
        {L"Cell spacing", defaults::GapChoices(), s.gap, IDM_GAP_FIRST, L" px"},
    };

    for (const NumberMenu& entry : numberMenus) {
        HMENU submenu = CreatePopupMenu();
        ownedSubmenus->push_back(submenu);

        for (size_t i = 0; i < entry.choices.size(); ++i) {
            wchar_t text[32];
            swprintf(text, 32, L"%d%s", entry.choices[i], entry.suffix);
            AppendItem(submenu,
                       MF_STRING |
                           (entry.choices[i] == entry.current ? MF_CHECKED : MF_UNCHECKED),
                       entry.firstId + i, text);
        }

        wchar_t title[96];
        if (entry.firstId == IDM_CELL_FIRST && metrics_.fitted) {
            // Say so when the taskbar forced the height down, rather than
            // leaving the user wondering why their choice did not apply.
            swprintf(title, 96, L"%s: %d px (height fit to %d)", entry.label,
                     entry.current, metrics_.cellHeight);
        } else {
            swprintf(title, 96, L"%s: %d%s", entry.label, entry.current, entry.suffix);
        }

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(submenu), title);
    }

    AppendSeparator(menu);

    // Move mode is armed here rather than being always-on, so a stray drag on
    // the taskbar cannot quietly relocate the widget.
    const bool moving = app_ && app_->IsMoveMode();
    AppendItem(menu, MF_STRING | (moving ? MF_CHECKED : MF_UNCHECKED), IDM_MOVE_WIDGET,
               moving ? L"Move widget — drag it now"
                      : L"Move widget…");
    AppendItem(menu, MF_STRING, IDM_RESET_POSITION, L"Reset widget position");

    AppendSeparator(menu);

    AppendItem(menu, MF_STRING, IDM_PING_NOW, L"Ping now");
    AppendItem(menu, MF_STRING, IDM_CLEAR_HISTORY, L"Clear monitor history");

    // Save profile: new, or overwrite an existing one.
    HMENU saveMenu = CreatePopupMenu();
    ownedSubmenus->push_back(saveMenu);
    AppendItem(saveMenu, MF_STRING, IDM_SAVE_NEW_PROFILE, L"New profile…");

    const std::vector<std::wstring> profileNames = profiles::Names();
    if (!profileNames.empty()) {
        AppendSeparator(saveMenu);
        for (size_t i = 0; i < profileNames.size() && i < 100; ++i) {
            AppendItem(saveMenu, MF_STRING, IDM_SAVE_PROFILE_FIRST + i,
                       profileNames[i].c_str());
        }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(saveMenu),
                L"Save monitor profile");

    // Load profile, with a Delete submenu beside it.
    //
    // macOS put an X button on each row via a custom NSView. Win32 menu items
    // cannot hold child controls, so deletion is its own submenu instead —
    // the same capability, one extra level down.
    if (profileNames.empty()) {
        // Greyed out with no submenu at all. Offering a submenu that only says
        // "No saved profiles" makes the user travel to find out there is
        // nothing there; a disabled item says the same thing up front.
        AppendItem(menu, MF_STRING | MF_GRAYED, 0, L"Load monitor profile");
    } else {
        HMENU loadMenu = CreatePopupMenu();
        ownedSubmenus->push_back(loadMenu);

        // Owner-drawn so each row can carry its own ✕, the way the macOS
        // version's custom row view did. The item data is the index into the
        // name list the app draws from.
        if (app_) app_->SetProfileMenuNames(profileNames);

        for (size_t i = 0; i < profileNames.size() && i < 100; ++i) {
            // Item data is the index plus one: zero would be indistinguishable
            // from "no data set" if another owner-draw item is ever added.
            AppendMenuW(loadMenu, MF_OWNERDRAW, IDM_LOAD_PROFILE_FIRST + i,
                        reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(i + 1)));
        }

        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(loadMenu),
                    L"Load monitor profile");
    }

    AppendItem(menu, MF_STRING, IDM_RESTORE_DEFAULTS, L"Restore monitor defaults");

    AppendSeparator(menu);

    const bool canAdd = app_ && app_->MonitorCount() < defaults::kMaxMonitors;
    AppendItem(menu, MF_STRING | (canAdd ? MF_ENABLED : MF_GRAYED), IDM_DUPLICATE,
               L"Duplicate this monitor");

    const bool canRemove = app_ && app_->MonitorCount() > 1;
    AppendItem(menu, MF_STRING | (canRemove ? MF_ENABLED : MF_GRAYED), IDM_REMOVE,
               L"Remove this monitor");

    AppendSeparator(menu);
    AppendItem(menu, MF_STRING, IDM_QUIT, L"Quit Pinger");

    return menu;
}

void MonitorController::ShowMenu(POINT screenPoint) {
    std::vector<HMENU> submenus;
    ScopedMenu menu(BuildMenu(&submenus));

    if (!menu) {
        // The root was never created, so nothing owns the submenus that had
        // already been made. Destroying the root would normally take the whole
        // tree with it.
        for (HMENU submenu : submenus) {
            if (submenu) DestroyMenu(submenu);
        }
        return;
    }

    // Menus are owned by the app's hidden top-level window, not by the widget.
    //
    // TrackPopupMenu requires its owner to be the foreground window, and
    // SetForegroundWindow silently fails on a WS_CHILD — which the widget is,
    // being parented into the taskbar. That failure is what made the first
    // click do nothing and only the second open the menu.
    HWND owner = app_ ? app_->MenuOwner() : window_;
    if (!owner) owner = window_;

    // Start from a clean slate: a row rectangle captured by a previous menu
    // must not be able to turn a plain click into a delete.
    if (app_) app_->ClearMenuSelection();

    SetForegroundWindow(owner);

    // TPM_NONOTIFY is deliberately absent: the owner needs WM_MENUSELECT to
    // capture the highlighted row's rectangle, and WM_MEASUREITEM/WM_DRAWITEM
    // to render the profile rows.
    const int command =
        TrackPopupMenuEx(menu.get(), TPM_RIGHTBUTTON | TPM_RETURNCMD,
                         screenPoint.x, screenPoint.y, owner, nullptr);

    // Dismisses the internal menu-mode state Win32 otherwise leaves behind,
    // which would swallow the next click.
    PostMessageW(owner, WM_NULL, 0, 0);

    // Every submenu here was attached with MF_POPUP, so the ScopedMenu above
    // destroys the whole tree when it goes out of scope.

    if (command > 0) HandleCommand(command);
}

// ------------------------------------------------------------------ commands

void MonitorController::HandleCommand(int command) {
    MonitorSettings& s = record_.settings;

    // Dialogs are owned by the app's hidden top-level window for the same
    // reason menus are. A modal dialog disables its owner, and Windows resolves
    // a WS_CHILD owner to its top-level ancestor — which here is Shell_TrayWnd,
    // so passing the widget would disable the entire taskbar for as long as a
    // prompt was open.
    HWND owner = app_ ? app_->MenuOwner() : window_;
    if (!owner) owner = window_;

    // Ranged commands first, so the switch below stays readable.
    const auto& intervalChoices = defaults::IntervalChoices();
    if (command >= IDM_INTERVAL_FIRST && command <= IDM_INTERVAL_LAST) {
        const size_t index = static_cast<size_t>(command - IDM_INTERVAL_FIRST);
        if (index < intervalChoices.size()) {
            s.interval = intervalChoices[index];
            PersistSettings();
            Restart();
        }
        return;
    }

    if (command >= IDM_SUCCESS_FIRST && command <= IDM_FAILURE_LAST) {
        const bool isSuccess = command <= IDM_SUCCESS_LAST;
        const size_t index = static_cast<size_t>(
            command - (isSuccess ? IDM_SUCCESS_FIRST : IDM_FAILURE_FIRST));

        const auto& presets = ColorPresets();
        if (index < presets.size()) {
            COLORREF color = 0;
            if (ColorFromHex(presets[index].hex, &color)) {
                s.SetColorFor(isSuccess ? ColorSlot::Success : ColorSlot::Failure, color);
                PersistSettings();
                Invalidate();
            }
        }
        return;
    }

    if (command >= IDM_TEXTSIZE_FIRST && command <= IDM_TEXTSIZE_LAST) {
        const size_t index = static_cast<size_t>(command - IDM_TEXTSIZE_FIRST);
        const auto& sizeChoices = defaults::TextSizeChoices();
        if (index < sizeChoices.size()) {
            s.textSize = sizeChoices[index];
            s.Sanitise();
            PersistSettings();
            // Relayout rebuilds the font, re-measures the readout and resizes
            // the widget, all of which change together.
            if (app_) app_->Relayout();
        }
        return;
    }

    struct RangeTarget {
        int  first;
        int  last;
        const std::vector<int>& choices;
        int* field;
    };

    const RangeTarget ranges[] = {
        {IDM_ROWS_FIRST, IDM_ROWS_LAST, defaults::RowChoices(), &s.rows},
        {IDM_COLUMNS_FIRST, IDM_COLUMNS_LAST, defaults::ColumnChoices(), &s.columns},
        {IDM_CELL_FIRST, IDM_CELL_LAST, defaults::CellChoices(), &s.cell},
        {IDM_GAP_FIRST, IDM_GAP_LAST, defaults::GapChoices(), &s.gap},
    };

    for (const RangeTarget& range : ranges) {
        if (command >= range.first && command <= range.last) {
            const size_t index = static_cast<size_t>(command - range.first);
            if (index < range.choices.size()) {
                *range.field = range.choices[index];
                s.Sanitise();
                PersistSettings();
                TrimSamples();
                if (app_) app_->Relayout();
            }
            return;
        }
    }

    // Only the three profile ranges need the (sorted) name list, so it is built
    // here rather than at the top — this function also runs for Quit and Ping
    // now, which have no business sorting profile names.
    const bool needsProfileNames =
        (command >= IDM_LOAD_PROFILE_FIRST && command <= IDM_DEL_PROFILE_LAST);
    const std::vector<std::wstring> profileNames =
        needsProfileNames ? profiles::Names() : std::vector<std::wstring>();

    if (command >= IDM_LOAD_PROFILE_FIRST && command <= IDM_LOAD_PROFILE_LAST) {
        const size_t index = static_cast<size_t>(command - IDM_LOAD_PROFILE_FIRST);

        // One command id, two meanings, decided by where in the row the click
        // landed. Menus report only the item, so the app captured the row's
        // screen rect while it was still highlighted.
        if (app_ && app_->LastSelectionHitDeleteGlyph()) {
            if (index < profileNames.size()) {
                const std::wstring& name = profileNames[index];
                if (Confirm(owner, L"Delete “" + name + L"”?",
                            L"The saved profile is removed. Monitors currently using "
                            L"those settings keep them — only the profile goes away.")) {
                    profiles::Delete(name);
                }
            }
            return;
        }

        if (index < profileNames.size()) {
            MonitorSettings loaded;
            if (profiles::Snapshot(profileNames[index], &loaded)) {
                loaded.Sanitise();
                s = loaded;
                PersistSettings();
                ClearHistory();
                if (app_) app_->Relayout();
                Restart();   // the host or frequency may have changed
            }
        }
        return;
    }

    if (command >= IDM_SAVE_PROFILE_FIRST && command <= IDM_SAVE_PROFILE_LAST) {
        const size_t index = static_cast<size_t>(command - IDM_SAVE_PROFILE_FIRST);
        if (index < profileNames.size()) {
            const std::wstring& name = profileNames[index];
            if (Confirm(owner, L"Overwrite “" + name + L"”?",
                        L"Replace that profile with this monitor's current settings?")) {
                profiles::Save(name, s);
            }
        }
        return;
    }

    switch (command) {
        case IDM_ABOUT:
            ShellExecuteW(nullptr, L"open", defaults::kHomepage, nullptr, nullptr,
                          SW_SHOWNORMAL);
            break;

        case IDM_SET_HOST: {
            std::wstring entered;
            std::wstring message = L"Enter an IP address or hostname to ping every " +
                                   FormatInterval(s.interval) + L".";
            if (!PromptForText(owner, L"Ping target", message, s.host, &entered)) break;

            const std::wstring newHost = entered.empty() ? defaults::kHost : entered;
            if (newHost == s.host) break;

            s.host = newHost;
            PersistSettings();
            ClearHistory();
            Restart();
            break;
        }

        case IDM_INTERVAL_CUSTOM: {
            std::wstring entered;
            wchar_t current[32];
            swprintf(current, 32, L"%g", s.interval);

            if (!PromptForText(owner, L"Ping frequency",
                               L"Seconds between pings (0.25–3600).", current,
                               &entered)) {
                break;
            }

            // Tolerate a trailing "s", since the menu displays one.
            std::wstring cleaned;
            for (wchar_t c : entered) {
                if (c != L's' && c != L'S') cleaned += c;
            }

            wchar_t* end = nullptr;
            const double seconds = wcstod(cleaned.c_str(), &end);
            if (end == cleaned.c_str() || !(seconds >= defaults::kMinInterval &&
                                            seconds <= defaults::kMaxInterval)) {
                break;
            }

            s.interval = seconds;
            PersistSettings();
            Restart();
            break;
        }

        case IDM_TOGGLE_LATENCY:
            s.showLatency = !s.showLatency;
            // A count left over from before the readout was hidden would make
            // the first shrink after re-enabling it fire early.
            shrinkCandidates_ = 0;
            PersistSettings();
            if (app_) app_->Relayout();
            break;

        case IDM_TOGGLE_FILL:
            s.fillHorizontal = !s.fillHorizontal;
            PersistSettings();
            // Only the paint order changes — same cells, same rolling window,
            // same average — so a repaint is all that is needed. The history
            // stays put and simply re-flows into the new direction.
            Invalidate();
            break;

        case IDM_SUCCESS_CUSTOM:
        case IDM_FAILURE_CUSTOM: {
            const ColorSlot slot =
                command == IDM_SUCCESS_CUSTOM ? ColorSlot::Success : ColorSlot::Failure;
            COLORREF chosen = 0;
            if (PickColor(owner, s.ColorFor(slot), &chosen)) {
                s.SetColorFor(slot, chosen);
                PersistSettings();
                Invalidate();
            }
            break;
        }

        case IDM_MOVE_WIDGET:
            if (app_) app_->BeginMoveMode();
            break;

        case IDM_RESET_POSITION:
            if (app_) app_->ResetWidgetPosition();
            break;

        case IDM_PING_NOW:
            Restart();
            break;

        case IDM_CLEAR_HISTORY:
            ClearHistory();
            break;

        case IDM_SAVE_NEW_PROFILE: {
            std::wstring name;
            if (!PromptForText(owner, L"Save monitor profile",
                               L"Saves this monitor's host, frequency, colors and "
                               L"grid shape.",
                               s.host, &name)) {
                break;
            }
            if (name.empty()) break;

            MonitorSettings existing;
            if (profiles::Snapshot(name, &existing)) {
                if (!Confirm(owner, L"Overwrite “" + name + L"”?",
                             L"A profile with that name already exists. Replace it "
                             L"with the current settings?")) {
                    break;
                }
            }
            profiles::Save(name, s);
            break;
        }

        case IDM_RESTORE_DEFAULTS: {
            MonitorSettings fresh;
            s = fresh;
            PersistSettings();
            ClearHistory();
            if (app_) app_->Relayout();
            Restart();
            break;
        }

        case IDM_DUPLICATE:
            if (app_) app_->DuplicateMonitor(this);
            break;

        case IDM_REMOVE:
            if (app_) app_->RemoveMonitor(this);
            break;

        case IDM_QUIT:
            if (app_) app_->RequestQuit();
            break;

        default:
            break;
    }
}

}  // namespace pinger
