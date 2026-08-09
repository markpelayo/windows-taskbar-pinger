// autostart.cpp — the HKCU Run entry behind "Run at startup".

#include "autostart.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace pinger {
namespace autostart {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// The value name shown in Task Manager's Startup tab, so make it recognisable.
constexpr wchar_t kValueName[] = L"Pinger";

constexpr wchar_t kDelayFlag[] = L"--delay=";

// Offered in the menu. Long enough to be worth having at the short end, and
// stopping at a minute because past that you are no longer avoiding the boot
// storm, you are just starting late.
constexpr int kDelays[] = {5, 10, 15, 20, 30, 60};

// A delay longer than this is almost certainly a typo in a hand-edited registry
// value, and honouring it would look like the app failing to start.
constexpr int kMaxDelaySeconds = 600;

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
std::wstring BuildCommand(int delaySeconds) {
    const std::wstring path = ExecutablePath();
    if (path.empty()) return L"";

    std::wstring command = L"\"" + path + L"\"";

    if (delaySeconds > 0) {
        wchar_t flag[32];
        swprintf(flag, 32, L" %s%d", kDelayFlag, delaySeconds);
        command += flag;
    }

    return command;
}

// Reads the current value, or an empty string when it is absent.
std::wstring ReadRegisteredCommand() {
    DWORD bytes = 0;
    LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName,
                                  RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status != ERROR_SUCCESS || bytes == 0) return L"";

    std::wstring value(bytes / sizeof(wchar_t), L'\0');

    // Re-state the buffer size from the buffer itself. REG_SZ byte counts are
    // always even in practice, but a hand-written odd one would otherwise leave
    // pcbData claiming one byte more than was allocated.
    bytes = static_cast<DWORD>(value.size() * sizeof(wchar_t));

    status = RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ,
                          nullptr, value.data(), &bytes);
    if (status != ERROR_SUCCESS) return L"";

    // RegGetValueW reports the size including the terminator; trim it so string
    // comparisons against a freshly built command actually match.
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

}  // namespace

const int* DelayChoices(int* count) {
    if (count) *count = static_cast<int>(sizeof(kDelays) / sizeof(kDelays[0]));
    return kDelays;
}

int DelayFromCommandLine(const wchar_t* commandLine) {
    if (!commandLine) return 0;

    const std::wstring line = commandLine;

    // Search past the quoted executable path. A directory literally named
    // "--delay=5" would otherwise be read as a delay that the entry point could
    // never honour, since the command line it receives excludes the path.
    const size_t closingQuote = line.rfind(L'"');
    const size_t from = (closingQuote == std::wstring::npos) ? 0 : closingQuote + 1;

    const size_t at = line.find(kDelayFlag, from);
    if (at == std::wstring::npos) return 0;

    const wchar_t* digits = line.c_str() + at + wcslen(kDelayFlag);
    wchar_t* end = nullptr;
    const long seconds = wcstol(digits, &end, 10);

    if (end == digits || seconds <= 0) return 0;
    return static_cast<int>(std::min<long>(seconds, kMaxDelaySeconds));
}

Config Current() {
    Config config;

    const std::wstring command = ReadRegisteredCommand();
    if (command.empty()) return config;

    config.enabled = true;
    config.delaySeconds = DelayFromCommandLine(command.c_str());
    return config;
}

bool SetEnabled(bool enabled, int delaySeconds) {
    if (!enabled) {
        const LSTATUS status =
            RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
        // Already absent counts as success — the caller asked for a state, not
        // for a change.
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    const int clamped = std::clamp(delaySeconds, 0, kMaxDelaySeconds);
    const std::wstring command = BuildCommand(clamped);
    if (command.empty()) return false;

    const LSTATUS status = RegSetKeyValueW(
        HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));

    return status == ERROR_SUCCESS;
}

void RepairPathIfNeeded() {
    const std::wstring registered = ReadRegisteredCommand();
    if (registered.empty()) return;   // not enabled; nothing to repair

    // Rebuild what the command *should* look like, keeping whatever delay the
    // user chose, and only write when it differs.
    const int delay = DelayFromCommandLine(registered.c_str());
    const std::wstring current = BuildCommand(delay);

    if (current.empty() || registered == current) return;

    // The user asked for "run at startup", not for that particular copy on that
    // particular path. Moving the folder should not quietly disable it.
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, current.c_str(),
                    static_cast<DWORD>((current.size() + 1) * sizeof(wchar_t)));
}

}  // namespace autostart
}  // namespace pinger
