// main.cpp — entry point.

#include <windows.h>
#include <objbase.h>

#include "app.h"
#include "autostart.h"

namespace {

// Only one copy should run: two would fight over the same taskbar slot and
// double every widget on screen.
constexpr wchar_t kMutexName[] = L"Local\\PingerTaskbarWidget.SingleInstance";

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance,
                      _In_opt_ HINSTANCE previousInstance,
                      _In_ LPWSTR commandLine,
                      _In_ int showCommand) {
    (void)previousInstance;
    (void)showCommand;

    pinger::ScopedHandle single(CreateMutexW(nullptr, TRUE, kMutexName));
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        // Already running. Exiting quietly is friendlier than an error box for
        // something a user may well have put in their Startup folder.
        return 0;
    }

    // Needed for CoCreateGuid and SHGetKnownFolderPath. Apartment-threaded
    // because this process is a UI thread with a message loop.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool comInitialised = SUCCEEDED(com);

    // If the app was registered to run at startup and has since been moved, the
    // stored path points at nothing. Fix it now rather than let the user
    // discover it after their next reboot.
    pinger::autostart::RepairPathIfNeeded();

    // Honour --delay=<seconds>, which the startup entry adds when the user picks
    // a delay. Sleeping here rather than after Initialise is the entire point:
    // what costs anything at boot is creating windows, embedding into the
    // taskbar, allocating GDI objects, resolving DNS and sending the first
    // packets — none of which have happened yet.
    //
    // Only when actually launched at startup, never when run by hand: the flag
    // is on the registered command line, not something a person types.
    const int delaySeconds = pinger::autostart::DelayFromCommandLine(commandLine);
    if (delaySeconds > 0) {
        Sleep(static_cast<DWORD>(delaySeconds) * 1000);
    }

    int exitCode = 1;
    {
        pinger::App app;
        if (app.Initialise(instance)) {
            exitCode = app.Run();
        } else {
            MessageBoxW(nullptr,
                        L"Pinger could not create its window, so there is nothing "
                        L"to show.\n\nThis usually means the desktop session is "
                        L"shutting down or a policy is blocking window creation.",
                        L"Pinger", MB_OK | MB_ICONERROR);
        }
        // The App destructor tears down monitors and the tray icon here, before
        // COM is uninitialised below.
    }

    if (comInitialised) CoUninitialize();
    return exitCode;
}
