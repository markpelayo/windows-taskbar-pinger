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

#include <memory>
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
    // already torn down are discarded rather than drawn. Drawn from a
    // process-wide counter, so an id is never ambiguous between two monitors.
    unsigned session = 0;
};

// Posted to the owning window whenever a packet settles.
// wParam = monitor index, lParam = new PingResult* (receiver takes ownership).
inline constexpr UINT WM_PINGER_RESULT = WM_APP + 1;

// Everything the worker thread touches.
//
// Held by shared_ptr and co-owned by the thread, which is what makes the
// wedged-worker case safe. If Stop() ever gives up waiting, the UI side simply
// drops its reference: the worker keeps the block — and its stop event — alive
// until it finally returns, and can never dereference freed memory.
//
// Without this the worker would be reading `window_` and interlocking on
// `monitorIndex_` inside a PingSession its owner had already destroyed, which
// is heap corruption rather than merely a stale read.
struct PingShared {
    HWND          window = nullptr;
    // Written by the UI thread when monitors are renumbered, read by the worker
    // on every packet, hence interlocked rather than a plain unsigned.
    volatile LONG monitorIndex = 0;
    ScopedHandle  stop;
    std::wstring  host;
    double        interval = 1.0;
    unsigned      session = 0;
};

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

    // Signals the worker to finish and waits briefly for it. Called on teardown
    // and before any restart, so a monitor never has two threads pinging at
    // once. Never blocks for long: see the definition.
    void Stop();

    // Identifier of the session currently running, for stale-result filtering.
    unsigned CurrentSession() const { return shared_ ? shared_->session : 0; }

    // Monitors are renumbered when one is removed, and the worker echoes this
    // value back as wParam. Without this the surviving workers keep posting
    // their old indices: one grid freezes and another receives someone else's
    // results.
    void SetMonitorIndex(unsigned index);

private:
    static DWORD WINAPI ThreadMain(LPVOID parameter);
    static void Run(const std::shared_ptr<PingShared>& shared);

    HWND     window_ = nullptr;
    unsigned monitorIndex_ = 0;

    std::shared_ptr<PingShared> shared_;
    ScopedHandle                thread_;
};

}  // namespace pinger
