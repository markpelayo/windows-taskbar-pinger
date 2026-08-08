// ping.h — one ping session per monitor.
//
// The macOS version spawns `ping -i <interval> -O` once per monitor and parses
// its output, specifically to avoid a fork/exec per packet. Windows has a better
// option: IcmpSendEcho in iphlpapi sends a single echo request from inside our
// own process. No child process at all, no output parsing, no admin rights
// (unlike a hand-rolled raw socket), and the round trip comes back as a number
// rather than as text we would have to scrape out of a localised console line.
//
// The cadence that `ping -i` gave us for free becomes one small worker thread
// per monitor: send, wait for the reply or the timeout, sleep the remainder of
// the interval, repeat. At most eight of these exist, each with a 64 KB stack,
// and each spends essentially all of its life blocked.

#pragma once

// Winsock must be pulled in before windows.h anywhere it is reachable,
// otherwise the older winsock.h that windows.h includes wins and every socket
// type is defined twice. WIN32_LEAN_AND_MEAN currently hides this, but relying
// on a build flag for correctness is how it breaks later.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

#include <string>

#include "raii.h"

namespace pinger {

// One packet's outcome.
struct PingResult {
    bool     reachable = false;
    bool     hasLatency = false;
    double   milliseconds = 0.0;
    // Monotonically increasing per session, so a controller can tell a stale
    // result from a current one after the host or interval changes.
    unsigned sequence = 0;
    // Identifies which session produced this, so results from a session we have
    // already torn down are discarded rather than drawn.
    unsigned session = 0;
};

// Posted to the owning window whenever a packet settles.
// wParam = monitor index, lParam = new PingResult* (receiver takes ownership).
inline constexpr UINT WM_PINGER_RESULT = WM_APP + 1;

class PingSession {
public:
    // `window` receives WM_PINGER_RESULT; `monitorIndex` is echoed back in
    // wParam so one window can host several sessions.
    PingSession(HWND window, unsigned monitorIndex);
    ~PingSession();

    PingSession(const PingSession&) = delete;
    PingSession& operator=(const PingSession&) = delete;

    // Stops any running session and starts a fresh one. Safe to call repeatedly;
    // this is also how "Ping now" forces an immediate packet.
    void Start(const std::wstring& host, double interval);

    // Signals the worker to finish and waits for it. Called on teardown, and
    // before any restart, so a monitor never has two threads pinging at once.
    void Stop();

    // Identifier of the session currently running, for stale-result filtering.
    // Drawn from a process-wide counter, so an id is never ambiguous between
    // two monitors — a per-session counter starting at zero in every session
    // would collide on the first packet of every monitor.
    unsigned CurrentSession() const { return session_; }

    // Monitors are renumbered when one is removed, and the worker echoes this
    // value back as wParam. Without this the surviving workers keep posting
    // their old indices: one grid freezes and another receives someone else's
    // results.
    void SetMonitorIndex(unsigned index);

private:
    static DWORD WINAPI ThreadMain(LPVOID parameter);
    void Run();

    HWND     window_ = nullptr;
    // Read by the worker thread on every packet and written by the UI thread on
    // a renumber, so it is interlocked rather than a plain unsigned.
    volatile LONG monitorIndex_ = 0;

    ScopedHandle thread_;
    ScopedHandle stopEvent_;

    // Set when Stop() gave up waiting for a wedged worker. That worker still
    // holds `this` and its own copy of the stop event, so this object must
    // never start a second thread, and must not be reused.
    bool abandoned_ = false;

    // Written only between Stop() and Start(), read by the worker thread it
    // belongs to, so no lock is needed on these.
    std::wstring host_;
    double       interval_ = 1.0;
    unsigned     session_ = 0;
};

}  // namespace pinger
