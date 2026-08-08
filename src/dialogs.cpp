// dialogs.cpp — text prompt, confirmation, colour picker.

#include "dialogs.h"

#include <commdlg.h>

#include "resource.h"

namespace pinger {

namespace {

// Everything the prompt dialog needs, passed through DialogBoxParam's lParam.
struct PromptContext {
    const std::wstring* title;
    const std::wstring* message;
    const std::wstring* initialValue;
    std::wstring*       result;
};

std::wstring Trim(const std::wstring& text) {
    const wchar_t* kSpace = L" \t\r\n";
    const size_t first = text.find_first_not_of(kSpace);
    if (first == std::wstring::npos) return L"";
    const size_t last = text.find_last_not_of(kSpace);
    return text.substr(first, last - first + 1);
}

// Centres a window on the monitor holding the cursor, which on a multi-monitor
// desk is where the user is looking. Falls back to the primary screen.
void CenterOnCursorMonitor(HWND window) {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return;

    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return;

    RECT bounds{};
    if (!GetWindowRect(window, &bounds)) return;

    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    const int x = info.rcWork.left + (info.rcWork.right - info.rcWork.left - width) / 2;
    const int y = info.rcWork.top + (info.rcWork.bottom - info.rcWork.top - height) / 2;

    SetWindowPos(window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

INT_PTR CALLBACK PromptProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG: {
            auto* context = reinterpret_cast<PromptContext*>(lParam);
            SetWindowLongPtrW(dialog, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));

            if (context) {
                SetWindowTextW(dialog, context->title->c_str());
                SetDlgItemTextW(dialog, IDC_PROMPT_LABEL, context->message->c_str());
                SetDlgItemTextW(dialog, IDC_PROMPT_EDIT, context->initialValue->c_str());
            }

            // Select the whole value so typing replaces it, which is what
            // someone retyping a host address almost always wants.
            HWND edit = GetDlgItem(dialog, IDC_PROMPT_EDIT);
            if (edit) {
                SetFocus(edit);
                SendMessageW(edit, EM_SETSEL, 0, -1);
            }

            CenterOnCursorMonitor(dialog);
            SetForegroundWindow(dialog);

            return FALSE;   // focus was set explicitly above
        }

        case WM_COMMAND: {
            const int id = LOWORD(wParam);

            if (id == IDOK) {
                auto* context = reinterpret_cast<PromptContext*>(
                    GetWindowLongPtrW(dialog, GWLP_USERDATA));

                if (context && context->result) {
                    wchar_t buffer[512];
                    const UINT length =
                        GetDlgItemTextW(dialog, IDC_PROMPT_EDIT, buffer, 512);
                    *context->result = Trim(std::wstring(buffer, length));
                }

                EndDialog(dialog, IDOK);
                return TRUE;
            }

            if (id == IDCANCEL) {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            break;
        }

        case WM_CLOSE:
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        default:
            break;
    }

    return FALSE;
}

}  // namespace

bool PromptForText(HWND owner,
                   const std::wstring& title,
                   const std::wstring& message,
                   const std::wstring& initialValue,
                   std::wstring* value) {
    if (!value) return false;

    std::wstring result;
    PromptContext context{&title, &message, &initialValue, &result};

    const INT_PTR outcome =
        DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_PROMPT), owner,
                        &PromptProc, reinterpret_cast<LPARAM>(&context));

    if (outcome != IDOK) return false;

    *value = result;
    return true;
}

bool Confirm(HWND owner, const std::wstring& title, const std::wstring& message) {
    const int outcome = MessageBoxW(owner, message.c_str(), title.c_str(),
                                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 |
                                        MB_SETFOREGROUND | MB_TOPMOST);
    return outcome == IDYES;
}

bool PickColor(HWND owner, COLORREF current, COLORREF* chosen) {
    if (!chosen) return false;

    // Static so the user's mixed colours survive between openings, the way the
    // shared NSColorPanel does on macOS.
    static COLORREF customColors[16] = {
        RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
        RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
        RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
        RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255), RGB(255, 255, 255),
    };

    CHOOSECOLORW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = owner;
    options.rgbResult = current;
    options.lpCustColors = customColors;
    options.Flags = CC_FULLOPEN | CC_RGBINIT | CC_ANYCOLOR;

    if (!ChooseColorW(&options)) return false;

    *chosen = options.rgbResult;
    return true;
}

}  // namespace pinger
