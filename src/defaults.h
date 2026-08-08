// defaults.h — factory settings, menu choices and colour helpers.
//
// Mirrors `enum Defaults` in the macOS original. The one real difference is the
// bar height: on macOS the menu bar is a fixed ~20pt, on Windows the taskbar
// height varies with DPI, orientation and version, so it is measured at runtime
// and the constant here is only a fallback.

#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pinger {

// ---------------------------------------------------------------- constants

namespace defaults {

inline constexpr wchar_t kHost[]          = L"8.8.8.8";
inline constexpr wchar_t kSuccessHex[]    = L"#2777F8";   // blue
inline constexpr wchar_t kFailureHex[]    = L"#F03631";   // red
inline constexpr int     kRows            = 3;
inline constexpr int     kColumns         = 8;
inline constexpr int     kCell            = 6;            // logical px at 96 DPI
inline constexpr int     kGap             = 1;
inline constexpr double  kInterval        = 1.0;          // seconds
inline constexpr bool    kShowLatency     = true;

// Used only until the real taskbar height is known, and in floating mode.
inline constexpr int     kMaxBarHeight    = 24;

inline constexpr double  kMinInterval     = 0.25;
inline constexpr double  kMaxInterval     = 3600.0;
inline constexpr int     kMaxMonitors     = 8;

// Ping payload. 32 bytes is what Windows `ping.exe` sends, so results are
// directly comparable with what a user would see in a console.
inline constexpr int     kPayloadBytes    = 32;

inline constexpr wchar_t kProjectName[]   = L"windows-taskbar-pinger";
inline constexpr wchar_t kAuthor[]        = L"markpelayo";
inline constexpr wchar_t kHomepage[]      = L"https://github.com/markpelayo/macos-menubar-pinger";
inline constexpr wchar_t kVersion[]       = L"1.0.0";

inline const std::vector<int>&    RowChoices();
inline const std::vector<int>&    ColumnChoices();
inline const std::vector<int>&    CellChoices();
inline const std::vector<int>&    GapChoices();
inline const std::vector<double>& IntervalChoices();

inline const std::vector<int>& RowChoices() {
    static const std::vector<int> v{2, 3, 4, 5, 6, 7, 8};
    return v;
}
inline const std::vector<int>& ColumnChoices() {
    static const std::vector<int> v{4, 6, 8, 10, 12, 16, 20, 24, 32};
    return v;
}
inline const std::vector<int>& CellChoices() {
    static const std::vector<int> v{2, 3, 4, 5, 6, 8, 10, 12};
    return v;
}
inline const std::vector<int>& GapChoices() {
    static const std::vector<int> v{0, 1, 2, 3};
    return v;
}
inline const std::vector<double>& IntervalChoices() {
    static const std::vector<double> v{0.5, 1, 2, 3, 5, 10, 15, 30, 60};
    return v;
}

}  // namespace defaults

// ------------------------------------------------------------ colour helpers

struct ColorPreset {
    const wchar_t* name;
    const wchar_t* hex;
};

inline const std::vector<ColorPreset>& ColorPresets() {
    static const std::vector<ColorPreset> v{
        {L"Blue",   L"#2777F8"},
        {L"Cyan",   L"#22C7D6"},
        {L"Green",  L"#2BD145"},
        {L"Yellow", L"#F5C518"},
        {L"Orange", L"#F5851F"},
        {L"Red",    L"#F03631"},
        {L"Purple", L"#A45CF5"},
        {L"Pink",   L"#F55CA8"},
        {L"White",  L"#FFFFFF"},
        {L"Gray",   L"#8E8E93"},
    };
    return v;
}

// Which of a monitor's two colours an action refers to.
enum class ColorSlot { Success, Failure };

inline const wchar_t* ColorSlotLabel(ColorSlot slot) {
    return slot == ColorSlot::Success ? L"Success color" : L"Failure color";
}

// Parses "#RRGGBB" or "RRGGBB" into a COLORREF. Returns false when malformed,
// leaving `out` untouched, so callers can keep whatever default they had.
bool ColorFromHex(const std::wstring& hex, COLORREF* out);

// Formats a COLORREF back to "#RRGGBB" for the settings file.
std::wstring ColorToHex(COLORREF color);

// Colour of a cell that has not been measured yet. GDI has no alpha in
// FillRect, so this is a pre-blended mid grey rather than a translucent one.
inline constexpr COLORREF kEmptyCellLight = RGB(196, 196, 196);
inline constexpr COLORREF kEmptyCellDark  = RGB(72, 72, 72);

}  // namespace pinger
