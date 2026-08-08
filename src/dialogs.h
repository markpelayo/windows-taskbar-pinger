// dialogs.h — the three pieces of UI that are not the widget itself.
//
// AppKit gave the macOS version NSAlert with an accessory view and a shared
// NSColorPanel. Win32 has MessageBox and ChooseColor built in, but no text
// prompt, so that one is a small dialog of our own.

#pragma once

#include <windows.h>

#include <string>

namespace pinger {

// A modal text prompt. Returns false when cancelled, leaving `value` untouched.
// Returns true on OK, with the trimmed text — which may be empty, so callers
// that need a value must check for that themselves.
bool PromptForText(HWND owner,
                   const std::wstring& title,
                   const std::wstring& message,
                   const std::wstring& initialValue,
                   std::wstring* value);

// A modal yes/no confirmation. Returns true when the user chooses yes.
bool Confirm(HWND owner, const std::wstring& title, const std::wstring& message);

// The system colour picker, seeded with `current`. Returns false when cancelled.
//
// The 16 custom colour slots are process-wide and deliberately shared, so a
// colour mixed for one monitor is still there when configuring another.
bool PickColor(HWND owner, COLORREF current, COLORREF* chosen);

}  // namespace pinger
