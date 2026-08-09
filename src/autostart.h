// autostart.h — "Run at startup".
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
// to turn this off without opening the app has an obvious place to do it — which
// a Startup-folder shortcut also offers, but a scheduled task would not.

#pragma once

#include <windows.h>

namespace pinger {
namespace autostart {

// True when the app is registered to start with Windows.
bool IsEnabled();

// Registers or unregisters. Returns false if the registry rejected the change,
// which realistically means group policy is locking the key down.
bool SetEnabled(bool enabled);

// Rewrites the registered path when it no longer matches where this executable
// actually is — the app has been moved or replaced since it was enabled.
//
// Called once at startup. Without it, moving the folder leaves an entry pointing
// at nothing and the app silently stops starting with Windows, which the user
// would only discover after their next reboot.
void RepairPathIfNeeded();

}  // namespace autostart
}  // namespace pinger
