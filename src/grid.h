// grid.h — drawing the rolling cell grid.
//
// The whole point of the app is here: one cell per ping, filling bottom-left
// upward then column by column, rolling over once full.
//
// Everything in this file exists to make that redraw cost nothing. The widget
// repaints once a second and is meant to run for weeks, so:
//
//   * brushes are cached and only rebuilt when a colour actually changes,
//   * the back buffer is reused and only reallocated when the grid changes
//     shape or the DPI changes,
//   * nothing is allocated per frame.
//
// A single leaked GDI object per redraw would exhaust the default 10,000 handle
// quota in under three hours.

#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "raii.h"
#include "settings.h"

namespace pinger {

// One cell's worth of history. Cells past the end of the sample list have not
// been measured yet and draw in the empty colour.
struct Sample {
    bool   reachable = false;
    bool   hasLatency = false;
    double milliseconds = 0.0;
};

// Pixel geometry of a grid at a given DPI. Cell width is always what the user
// asked for; height shrinks to fit when the taskbar is too short for
// rows x (cell + gap), exactly as the macOS version fits the menu bar.
struct GridMetrics {
    int cellWidth = 0;
    int cellHeight = 0;
    int gap = 0;
    int width = 0;
    int height = 0;
    // True when the height had to be reduced to fit; the menu says so.
    bool fitted = false;
};

// Computes the geometry for these settings at this DPI and available height.
GridMetrics ComputeMetrics(const MonitorSettings& settings, int dpi, int availableHeight);

// Owns the cached GDI objects for one monitor's grid and paints it.
class GridRenderer {
public:
    GridRenderer() = default;
    ~GridRenderer() = default;

    GridRenderer(const GridRenderer&) = delete;
    GridRenderer& operator=(const GridRenderer&) = delete;

    // Paints the grid, and optionally the latency text, into `target`.
    //
    // `bounds` is the full client area. The grid is drawn left-aligned and
    // vertically centred; the text, when present, follows it.
    void Paint(HDC target,
               const RECT& bounds,
               const MonitorSettings& settings,
               const GridMetrics& metrics,
               const std::vector<Sample>& samples,
               const std::wstring& latencyText,
               COLORREF textColor,
               HFONT font,
               int dpi);

    // Drops every cached object. Called when a monitor goes away, so its
    // handles are released immediately rather than at process exit.
    void Reset();

private:
    void EnsureBrushes(COLORREF success, COLORREF failure, COLORREF empty);

    ScopedBrush successBrush_;
    ScopedBrush failureBrush_;
    ScopedBrush emptyBrush_;

    // The colours the cached brushes were built from, so we can tell when a
    // rebuild is actually needed rather than rebuilding every frame.
    COLORREF cachedSuccess_ = CLR_INVALID;
    COLORREF cachedFailure_ = CLR_INVALID;
    COLORREF cachedEmpty_ = CLR_INVALID;
};

// Renders a small rounded colour swatch for the colour menus.
//
// Menus are rebuilt every time they open, so these are cached by colour rather
// than recreated per item; the cache is bounded and cleared wholesale if a user
// somehow picks hundreds of custom colours.
HBITMAP SwatchForColor(COLORREF color, int dpi);

// Frees every cached swatch. Called on shutdown.
void ClearSwatchCache();

}  // namespace pinger
