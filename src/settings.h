// settings.h — the model, and its persistence.
//
// Replaces UserDefaults with a small INI file at %APPDATA%\Pinger\settings.ini.
// INI rather than JSON because a correct JSON parser is a few hundred lines we
// would have to write and test, while this format needs about eighty and is
// something a user can sensibly hand-edit when something goes wrong.
//
// Settings live in memory and are written through on change, so a redraw never
// touches the disk — the same rule the macOS version follows.

#pragma once

#include <windows.h>

#include <map>
#include <string>
#include <vector>

#include "defaults.h"

namespace pinger {

// Everything the user can configure on one monitor. This is the unit that is
// persisted, duplicated, and saved as a named profile.
struct MonitorSettings {
    std::wstring host       = defaults::kHost;
    COLORREF     success    = RGB(0x27, 0x77, 0xF8);
    COLORREF     failure    = RGB(0xF0, 0x36, 0x31);
    int          rows       = defaults::kRows;
    int          columns    = defaults::kColumns;
    int          cell       = defaults::kCell;
    int          gap        = defaults::kGap;
    double       interval   = defaults::kInterval;
    bool         showLatency = defaults::kShowLatency;
    // Point size of the latency readout. Per-monitor, like every other visual
    // setting, so two grids side by side can be weighted differently.
    int          textSize   = defaults::kTextSize;

    int CellCount() const { return rows * columns; }

    COLORREF ColorFor(ColorSlot slot) const {
        return slot == ColorSlot::Success ? success : failure;
    }

    void SetColorFor(ColorSlot slot, COLORREF color) {
        if (slot == ColorSlot::Success) {
            success = color;
        } else {
            failure = color;
        }
    }

    // Clamps anything a hand-edited file could have made nonsensical.
    void Sanitise();
};

// One saved monitor: an identity plus its settings.
struct MonitorRecord {
    std::wstring    id;
    MonitorSettings settings;
};

// Where the widget sits along the taskbar. App-wide rather than per-monitor:
// the grids are laid out in one row, so there is one position to remember.
struct WidgetPlacement {
    // False means "park beside the notification area and recompute as it
    // changes width", which is the default. True means the user dragged it
    // somewhere and we should leave it there.
    bool manual = false;

    // Distance in logical pixels at 96 DPI from the taskbar's right edge to the
    // widget's right edge.
    //
    // Measured from the right rather than the left on purpose: the left edge is
    // where the Windows 11 weather widget lives and where the centred app
    // buttons expand from, so a left-anchored position drifts. Anchoring right
    // also survives a resolution change, which an absolute position does not.
    //
    // The trade-off, and it is a real one: this is the taskbar's edge, not the
    // notification area's, so a tray that grows an icon will creep toward the
    // widget until it is moved again.
    int offsetFromRight = 0;
};

// The whole persisted document.
struct SettingsDocument {
    std::vector<MonitorRecord> monitors;
    WidgetPlacement placement;
    // Kept in insertion order alongside a name, so profile names may contain
    // any character without needing to be escaped into a section header.
    std::vector<std::pair<std::wstring, MonitorSettings>> profiles;
};

// ------------------------------------------------------------------- storage

namespace store {

// Full path of the settings file, creating no directories.
std::wstring FilePath();

// Loads from disk once; later calls return the in-memory copy. A malformed file
// costs the user their settings, not the app — it starts clean rather than
// refusing to launch.
SettingsDocument& Document();

// Writes the document out atomically (temp file plus rename), so a crash
// mid-write cannot leave a truncated settings file behind. Failures are
// swallowed on purpose: a read-only profile directory must not take down a
// status widget.
void Save();

// Loads saved monitors, creating one default monitor on first run.
std::vector<MonitorRecord> LoadMonitors();

// Rewrites the monitor list to match what is on screen.
void PersistMonitors(const std::vector<MonitorRecord>& monitors);

// Records where the user dragged the widget to.
void PersistPlacement(const WidgetPlacement& placement);

}  // namespace store

// ------------------------------------------------------------------ profiles

namespace profiles {

// Profile names, natural-sorted so "Router 2" precedes "Router 10".
std::vector<std::wstring> Names();

// Returns a copy of the named profile's settings, or false when absent.
bool Snapshot(const std::wstring& name, MonitorSettings* out);

// Creates or overwrites.
void Save(const std::wstring& name, const MonitorSettings& settings);

void Delete(const std::wstring& name);

}  // namespace profiles

// ------------------------------------------------------------------ helpers

// Formats a ping interval the way the menu shows it: "1 s", "0.5 s", "1.25 s".
std::wstring FormatInterval(double seconds);

// Case-insensitive natural comparison, the equivalent of macOS's
// localizedStandardCompare, so numbered names sort the way people expect.
bool NaturalLess(const std::wstring& a, const std::wstring& b);

}  // namespace pinger
