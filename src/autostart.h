// autostart.h — "Run at startup", with an optional delay.
//
// Implemented as a value under HKCU\...\CurrentVersion\Run.
//
// Deliberately HKCU and not HKLM: the per-machine key would start the widget for
// every account on the computer and needs administrator rights to write, and a
// status indicator is not worth an elevation prompt. A Startup-folder shortcut
// would work too, but creating one means COM and IShellLink for something a
// single registry value expresses exactly.
//
// The Run key is also what Task Manager's Startup tab lists, so a user who wants
// to turn this off without opening the app has an obvious place to do it.
//
// ---------------------------------------------------------------- the delay
//
// The delay is a `--delay=<seconds>` argument on the registered command line,
// which the app honours before it creates any window or sends any packet.
//
// Be clear about what that does and does not buy. Windows still *launches* the
// process at sign-in — the delay cannot prevent that from a Run key. What it
// defers is everything the app actually costs: creating windows, embedding into
// the taskbar, GDI allocation, DNS resolution and the first ICMP packets. Those
// are the parts that would otherwise contend with the shell and with every other
// startup app for disk and network during the busiest few seconds of a boot.
//
// Genuinely preventing the launch would mean a Task Scheduler entry with a
// delayed trigger, which is a COM API, an XML task definition and a meaningfully
// larger surface for something that saves a few megabytes of working set for a
// few seconds. If that trade ever looks worth it, this is the place to change.

#pragma once

#include <windows.h>

namespace pinger {
namespace autostart {

// What is currently registered.
struct Config {
    bool enabled = false;
    // Seconds to wait before doing anything. Zero means start immediately.
    int  delaySeconds = 0;
};

// Reads the current registration.
Config Current();

// Registers or unregisters. `delaySeconds` is ignored when disabling, and
// clamped to something sane when enabling. Returns false if the registry
// rejected the change, which realistically means group policy is locking the
// key down.
bool SetEnabled(bool enabled, int delaySeconds);

// Rewrites the registered path when it no longer matches where this executable
// actually is — the app has been moved or replaced since it was enabled. The
// delay is preserved.
//
// Called once at startup. Without it, moving the folder leaves an entry pointing
// at nothing and the app silently stops starting with Windows, which the user
// would only discover after their next reboot.
void RepairPathIfNeeded();

// Parses `--delay=<seconds>` out of a command line, returning 0 when absent or
// malformed. Used by the entry point, which has no reason to touch the registry.
int DelayFromCommandLine(const wchar_t* commandLine);

// The delays the menu offers, in seconds.
const int* DelayChoices(int* count);

}  // namespace autostart
}  // namespace pinger
