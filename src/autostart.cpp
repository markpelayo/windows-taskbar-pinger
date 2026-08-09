// autostart.cpp — the HKCU Run entry behind "Run at startup".

#include "autostart.h"

#include <string>

namespace pinger {
namespace autostart {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The value name shown in Task Manager's Startup tab, so make it recognisable.
constexpr wchar_t kValueName[] = L"Pinger";

// Full path of this executable.
std::wstring ExecutablePath() {
    // MAX_PATH is not enough on a long-path-enabled system, so grow until it
    // fits rather than silently truncating — a truncated path in the Run key
    // would fail at the next boot with no indication why.
    std::wstring path(MAX_PATH, L'\0');

    for (;;) {
        const DWORD written =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

        if (written == 0) return L"";

        if (written < path.size()) {
            path.resize(written);
            return path;
        }

        if (path.size() >= 32768) return L"";   // the Win32 ceiling; give up
        path.resize(path.size() * 2);
    }
}

// The Run key runs its values as command lines, so a path containing a space —
// "C:\Program Files\..." being the obvious one — must be quoted or Windows will
// try to launch "C:\Program".
std::wstring QuotedExecutablePath() {
    const std::wstring path = ExecutablePath();
    if (path.empty()) return L"";
    return L"\"" + path + L"\"";
}

// Reads the current value, or an empty string when it is absent.
std::wstring ReadRegisteredCommand() {
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName,
                                  RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) return L"";

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
                          nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS) return L"";

    // RegGetValueW reports the size including the terminator; trim it so string
    // comparisons against a freshly built path actually match.
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

}  // namespace

bool IsEnabled() {
    return !ReadRegisteredCommand().empty();
}

bool SetEnabled(bool enabled) {
    if (!enabled) {
        const LSTATUS status =
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        // Already absent counts as success — the caller asked for a state, not
        // for a change.
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    const std::wstring command = QuotedExecutablePath();
    if (command.empty()) return false;

    const LSTATUS status = RegSetKeyValueW(
        HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));

    return status == ERROR_SUCCESS;
}

void RepairPathIfNeeded() {
    const std::wstring registered = ReadRegisteredCommand();
    if (registered.empty()) return;   // not enabled; nothing to repair

    const std::wstring current = QuotedExecutablePath();
    if (current.empty() || registered == current) return;

    // The user asked for "start with Windows", not for that particular copy on
    // that particular path. Moving the folder should not quietly disable it.
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, current.c_str(),
                    static_cast<DWORD>((current.size() + 1) * sizeof(wchar_t)));
}

}  // namespace autostart
}  // namespace pinger
