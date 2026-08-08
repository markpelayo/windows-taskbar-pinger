// grid.cpp — GDI rendering of the cell grid and the menu colour swatches.

#include "grid.h"

#include <algorithm>
#include <map>
#include <utility>

namespace pinger {

namespace {

// Scales a value authored at 96 DPI to the current DPI.
int Scale(int value, int dpi) {
    return MulDiv(value, dpi, 96);
}

}  // namespace

// ------------------------------------------------------------------ geometry

GridMetrics ComputeMetrics(const MonitorSettings& settings, int dpi, int availableHeight) {
    GridMetrics metrics;

    const int cell = std::max(1, Scale(settings.cell, dpi));
    const int gap = std::max(0, Scale(settings.gap, dpi));

    metrics.cellWidth = cell;
    metrics.cellHeight = cell;
    metrics.gap = gap;

    // rows cells plus the gaps between them.
    const int requested = settings.rows * (cell + gap) - gap;

    if (availableHeight > 0 && requested > availableHeight) {
        // Keep the width the user chose, shrink the height to fit. Matches the
        // macOS behaviour of fitting the grid into the menu bar.
        const int forGaps = gap * (settings.rows - 1);
        const int usable = availableHeight - forGaps;
        metrics.cellHeight = std::max(1, usable / std::max(1, settings.rows));
        metrics.fitted = true;
    }

    metrics.width = settings.columns * (metrics.cellWidth + gap) - gap;
    metrics.height = settings.rows * (metrics.cellHeight + gap) - gap;

    metrics.width = std::max(1, metrics.width);
    metrics.height = std::max(1, metrics.height);
    return metrics;
}

// ------------------------------------------------------------------ painting

void GridRenderer::EnsureBrushes(COLORREF success, COLORREF failure, COLORREF empty) {
    // Only rebuild what actually changed. In steady state this does nothing at
    // all, which is the point: no GDI object is created on a normal redraw.
    if (success != cachedSuccess_ || !successBrush_) {
        successBrush_.reset(CreateSolidBrush(success));
        cachedSuccess_ = success;
    }
    if (failure != cachedFailure_ || !failureBrush_) {
        failureBrush_.reset(CreateSolidBrush(failure));
        cachedFailure_ = failure;
    }
    if (empty != cachedEmpty_ || !emptyBrush_) {
        emptyBrush_.reset(CreateSolidBrush(empty));
        cachedEmpty_ = empty;
    }
}

void GridRenderer::Reset() {
    successBrush_.reset();
    failureBrush_.reset();
    emptyBrush_.reset();
    cachedSuccess_ = CLR_INVALID;
    cachedFailure_ = CLR_INVALID;
    cachedEmpty_ = CLR_INVALID;
}

void GridRenderer::Paint(HDC target,
                         const RECT& bounds,
                         const MonitorSettings& settings,
                         const GridMetrics& metrics,
                         const std::vector<Sample>& samples,
                         const std::wstring& latencyText,
                         COLORREF textColor,
                         HFONT font,
                         int dpi) {
    if (!target) return;

    // Unmeasured cells have to read against whichever taskbar theme is in use.
    // A light text colour means a dark taskbar, so the dim cells go lighter
    // than the background, and vice versa. Deriving it from the text colour
    // rather than reading a theme setting means it also follows a mid-session
    // light/dark switch, which arrives as WM_SETTINGCHANGE.
    const int textLuma = (GetRValue(textColor) * 299 + GetGValue(textColor) * 587 +
                          GetBValue(textColor) * 114) / 1000;
    const COLORREF empty = textLuma > 128 ? kEmptyCellDark : kEmptyCellLight;

    EnsureBrushes(settings.success, settings.failure, empty);

    const int boundsHeight = bounds.bottom - bounds.top;
    const int originX = bounds.left;
    const int originY = bounds.top + std::max(0, (boundsHeight - metrics.height) / 2);

    const int rows = std::max(1, settings.rows);
    const int count = settings.CellCount();
    const int measured = static_cast<int>(samples.size());

    for (int index = 0; index < count; ++index) {
        // Advance a column every `rows` samples, filling upward within it.
        const int column = index / rows;
        const int rowFromBottom = index % rows;
        // GDI's y axis grows downward, so the bottom row is drawn last.
        const int rowFromTop = rows - 1 - rowFromBottom;

        RECT cell;
        cell.left = originX + column * (metrics.cellWidth + metrics.gap);
        cell.top = originY + rowFromTop * (metrics.cellHeight + metrics.gap);
        cell.right = cell.left + metrics.cellWidth;
        cell.bottom = cell.top + metrics.cellHeight;

        HBRUSH brush;
        if (index < measured) {
            brush = samples[static_cast<size_t>(index)].reachable ? successBrush_.get()
                                                                  : failureBrush_.get();
        } else {
            brush = emptyBrush_.get();
        }

        if (brush) FillRect(target, &cell, brush);
    }

    if (latencyText.empty() || !font) return;

    RECT textRect = bounds;
    textRect.left = originX + metrics.width + Scale(6, dpi);

    if (textRect.left >= textRect.right) return;

    SelectGuard fontGuard(target, font);
    const int previousMode = SetBkMode(target, TRANSPARENT);
    const COLORREF previousColor = SetTextColor(target, textColor);

    DrawTextW(target, latencyText.c_str(), static_cast<int>(latencyText.size()),
              &textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    SetTextColor(target, previousColor);
    SetBkMode(target, previousMode);
}

// ------------------------------------------------------------------ swatches

namespace {

// Keyed by colour and DPI, since a swatch is drawn at a fixed logical size.
std::map<std::pair<COLORREF, int>, HBITMAP>& SwatchCache() {
    static std::map<std::pair<COLORREF, int>, HBITMAP> cache;
    return cache;
}

}  // namespace

HBITMAP SwatchForColor(COLORREF color, int dpi) {
    auto& cache = SwatchCache();
    const auto key = std::make_pair(color, dpi);

    const auto found = cache.find(key);
    if (found != cache.end()) return found->second;

    // Ten presets plus a handful of custom picks is the realistic ceiling.
    //
    // Past that, stop caching and return nothing rather than clearing: this is
    // called from inside menu construction, and earlier items in the very menu
    // being built already hold bitmaps from this cache. Deleting them here
    // would pull them out from under an open menu. A missing swatch is a
    // cosmetic loss on a pathological case; a dangling HBITMAP is not.
    if (cache.size() >= 64) return nullptr;

    const int size = std::max(8, Scale(12, dpi));

    ScopedWindowDC screen(nullptr, GetDC(nullptr));
    if (!screen) return nullptr;

    ScopedMemoryDC memory(CreateCompatibleDC(screen.get()));
    if (!memory) return nullptr;

    HBITMAP bitmap = CreateCompatibleBitmap(screen.get(), size, size);
    if (!bitmap) return nullptr;

    {
        SelectGuard bitmapGuard(memory.get(), bitmap);

        // Menu background, so the swatch does not sit on a black square.
        RECT full{0, 0, size, size};
        FillRect(memory.get(), &full, GetSysColorBrush(COLOR_MENU));

        ScopedBrush fill(CreateSolidBrush(color));
        ScopedPen border(CreatePen(PS_SOLID, 1, RGB(96, 96, 96)));

        if (fill && border) {
            SelectGuard fillGuard(memory.get(), fill.get());
            SelectGuard borderGuard(memory.get(), border.get());
            RoundRect(memory.get(), 0, 0, size, size, 4, 4);
        }
        // Both guards unwind here, restoring the DC's original objects before
        // the brush and pen are destroyed — deleting a selected object silently
        // fails and leaks it.
    }

    cache[key] = bitmap;
    return bitmap;
}

void ClearSwatchCache() {
    auto& cache = SwatchCache();
    for (auto& entry : cache) {
        if (entry.second) DeleteObject(entry.second);
    }
    cache.clear();
}

}  // namespace pinger
