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

    // Total width this widget wants, including the latency text when shown.
    int DesiredWidth() const;

    // Re-measures for the current DPI and available thickness, then repaints.
    void Layout(int dpi, int availableThickness);

    // Paints into the given DC; called from the host window's WM_PAINT.
    // `slot` is this monitor's share of the client area, and `font` is the
    // host's cached font — one font is shared by every grid.
    void Paint(HDC dc, const RECT& slot, HFONT font);

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
