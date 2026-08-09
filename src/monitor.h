// monitor.h — one grid: its window, its ping session, its menu.
//
// The direct equivalent of MonitorController in the macOS original, with
// NSStatusItem replaced by a small child window that lives in the taskbar.

#pragma once

#include <windows.h>

#include <climits>
#include <memory>
#include <string>
#include <vector>

#include "grid.h"
#include "ping.h"
#include "settings.h"

namespace pinger {

class App;

// True while a context menu is on screen.
//
// TrackPopupMenuEx runs its own modal message pump, so posted messages are
// dispatched while a menu is up — which is unsafe for anything that destroys a
// MonitorController, since its ShowMenu is still on the stack.
bool MenuIsOpen();

class MonitorController {
public:
    MonitorController(App* app, const MonitorRecord& record, HWND host, unsigned index);
    ~MonitorController();

    MonitorController(const MonitorController&) = delete;
    MonitorController& operator=(const MonitorController&) = delete;

    const std::wstring& Id() const { return record_.id; }
    MonitorSettings& Settings() { return record_.settings; }
    const MonitorSettings& Settings() const { return record_.settings; }
    const MonitorRecord& Record() const { return record_; }

    HWND Window() const { return window_; }
    unsigned Index() const { return index_; }

    // Must also tell the running ping session, which echoes the index back as
    // the wParam of every result it posts.
    void SetIndex(unsigned index);

    // Points this monitor at a newly created host window and restarts its ping
    // session against it. Needed when Explorer restarts: the widget is a child
    // of the taskbar, so the shell destroys it along with Shell_TrayWnd and a
    // replacement has to be built.
    void RebindWindow(HWND window);

    // Total width this widget wants, including the latency text when shown.
    int DesiredWidth() const;

    // Re-measures for the current DPI and available thickness, then repaints.
    void Layout(int dpi, int availableThickness);

    // Paints into the given DC; called from the host window's WM_PAINT.
    // `slot` is this monitor's share of the client area. `textColor` comes from
    // the app, which caches it rather than re-reading the theme every frame.
    void Paint(HDC dc, const RECT& slot, COLORREF textColor);

    // Handles one settled packet. Applies the opening-packet hold-back rule
    // before anything is drawn.
    void HandleResult(const PingResult& result);

    // Restarts the ping session, which also sends a packet immediately.
    void Restart();

    // Empties the grid and every counter derived from it.
    void ClearHistory();

    // Builds this monitor's context menu and runs it at the given screen point.
    void ShowMenu(POINT screenPoint);

    // Text for the hover tooltip.
    std::wstring TooltipText() const;

private:
    void CommitSample(const PingResult& result);
    void TrimSamples();
    void Invalidate();
    void PersistSettings();

    std::wstring AverageLatencyText() const;

    // Rebuilds the readout's font when its point size or the DPI changes.
    // Owned per monitor rather than shared by the app, because the size is a
    // per-monitor setting like the colours and the grid shape.
    void EnsureFont();

    // Measures the space the readout actually needs, rather than reserving a
    // worst case. See the definition for why this is not simply the width of
    // the current string.
    void MeasureLatencyWidth();
    bool HasAverageLatency() const { return latencyCount_ > 0; }
    double AverageLatency() const;

    // Menu construction, all rebuilt on open rather than kept live, so a ping
    // never touches menu state.
    HMENU BuildMenu(std::vector<HMENU>* ownedSubmenus);
    void HandleCommand(int command);

    App*            app_ = nullptr;
    MonitorRecord   record_;
    HWND            window_ = nullptr;
    unsigned        index_ = 0;

    std::unique_ptr<PingSession> session_;
    GridRenderer                 renderer_;
    GridMetrics                  metrics_;
    int                          dpi_ = 96;
    int                          availableThickness_ = 24;

    // Measured width of the latency readout, and the character count it was
    // measured for. The count is what decides when a re-measure is needed:
    // "5 ms" becoming "6 ms" changes nothing, "9 ms" becoming "10 ms" does.
    int    latencyWidth_ = 0;
    size_t latencyLength_ = 0;

    // How many consecutive samples have wanted a *narrower* readout. The width
    // grows immediately but only shrinks once this many agree, so a value
    // sitting on a digit boundary cannot make the widget resize every second.
    int              shrinkCandidates_ = 0;
    static constexpr int kShrinkHoldSamples = 10;

    // The readout's font, rebuilt only when its point size or the DPI changes.
    ScopedFont font_;
    int        fontDpi_ = 0;
    int        fontSize_ = 0;

    // Oldest sample first. Grows to CellCount(), then rolls: drop the oldest,
    // append the newest, so every cell shifts back one position.
    std::vector<Sample> samples_;

    // The first result of a session is only drawn once it is unambiguous.
    // A lone opening failure is routinely just route setup — ARP, DNS, a Wi-Fi
    // radio waking up — so it is held back rather than painting a red cell on
    // every launch. If the next packet replies it is discarded; if the next
    // packet also fails, the host really is unreachable and both are drawn.
    bool       firstOutcomeSettled_ = false;
    bool       holdingFirstFailure_ = false;
    PingResult heldFirstFailure_{};

    // Running totals over the visible window, so the average never costs a full
    // pass: add on append, subtract whatever rolls off the end.
    double latencySum_ = 0.0;
    int    latencyCount_ = 0;

};

}  // namespace pinger
