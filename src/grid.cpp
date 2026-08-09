// grid.cpp — GDI rendering of the cell grid and the menu colour swatches.

#include "grid.h"

#include <algorithm>

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

    const int columns = std::max(1, settings.columns);

    for (int index = 0; index < count; ++index) {
        int column = 0;
        int rowFromTop = 0;

        if (settings.fillHorizontal) {
            // Rows, read like text: left to right along the top row, then down.
            // The newest sample lands at the bottom right.
            rowFromTop = index / columns;
            column = index % columns;
        } else {
            // Columns, the macOS original's order: up from the bottom-left,
            // then on to the next column. The newest sample lands at the top
            // right. GDI's y axis grows downward, so a row counted from the
            // bottom has to be flipped before it is drawn.
            column = index / rows;
            const int rowFromBottom = index % rows;
            rowFromTop = rows - 1 - rowFromBottom;
        }

        // Only the mapping from sample index to cell position differs between
        // the two orders. The sample list, the rolling window and the running
        // latency average are all untouched — which is why switching direction
        // does not disturb the average.

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

// The swatch cache.
//
// A flat array rather than a std::map: the realistic population is ten presets
// plus a handful of custom picks, a linear scan of sixteen entries is free next
// to the CreateCompatibleBitmap it saves, and it keeps <map> — several KB of
// red-black tree code — out of the binary entirely.
//
// Keyed by colour *and* DPI, since a swatch is drawn at a fixed logical size.
struct SwatchEntry {
    COLORREF color = CLR_INVALID;
    int      dpi = 0;
    HBITMAP  bitmap = nullptr;
};

// Ten presets plus the two current colours fill twelve slots per DPI, so this
// has to hold at least two DPIs' worth or the swatches quietly vanish after the
// first display change.
constexpr size_t kSwatchCapacity = 32;

SwatchEntry g_swatches[kSwatchCapacity];
size_t g_swatchCount = 0;

}  // namespace

HBITMAP SwatchForColor(COLORREF color, int dpi) {
    for (size_t i = 0; i < g_swatchCount; ++i) {
        if (g_swatches[i].color == color && g_swatches[i].dpi == dpi) {
            return g_swatches[i].bitmap;
        }
    }

    // Full. Return nothing rather than evicting: this runs during menu
    // construction, and earlier items in the very menu being built already hold
    // bitmaps from this cache, so deleting one would pull it out from under an
    // open menu. A missing swatch is cosmetic; a dangling HBITMAP is not.
    // ClearSwatchCache, called only when no menu is up, is what reclaims them.
    if (g_swatchCount >= kSwatchCapacity) return nullptr;

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

    g_swatches[g_swatchCount].color = color;
    g_swatches[g_swatchCount].dpi = dpi;
    g_swatches[g_swatchCount].bitmap = bitmap;
    ++g_swatchCount;

    return bitmap;
}

void ClearSwatchCache() {
    for (size_t i = 0; i < g_swatchCount; ++i) {
        if (g_swatches[i].bitmap) DeleteObject(g_swatches[i].bitmap);
        g_swatches[i] = SwatchEntry{};
    }
    g_swatchCount = 0;
}

}  // namespace pinger
