// ping.cpp — ICMP echo via iphlpapi, one worker thread per monitor.

#include "ping.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// iphlpapi.h and icmpapi.h must follow the winsock headers.
#include <iphlpapi.h>
#include <icmpapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "defaults.h"

namespace pinger {

namespace {

// Session identifiers come from one process-wide counter so that no two
// monitors can ever produce the same id. A per-object counter starting at zero
// would make every monitor's first session indistinguishable, which defeats the
// stale-result filter it exists for.
volatile LONG g_nextSessionId = 0;

// IcmpCreateFile's handle is closed with IcmpCloseHandle, not CloseHandle.
// It happens to be a real file handle on current Windows, so CloseHandle
// appears to work — but that is undocumented and iphlpapi is free to change it.
class ScopedIcmpHandle {
public:
    ScopedIcmpHandle() = default;
    explicit ScopedIcmpHandle(HANDLE handle)
        : handle_(handle == INVALID_HANDLE_VALUE ? nullptr : handle) {}

    ~ScopedIcmpHandle() { reset(); }

    ScopedIcmpHandle(const ScopedIcmpHandle&) = delete;
    ScopedIcmpHandle& operator=(const ScopedIcmpHandle&) = delete;

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != handle) IcmpCloseHandle(handle_);
        handle_ = (handle == INVALID_HANDLE_VALUE) ? nullptr : handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// Winsock is initialised once for the process. getaddrinfo needs it; the ICMP
// APIs do not, but resolution and pinging always travel together here.
struct WinsockScope {
    WinsockScope() {
        WSADATA data{};
        ok = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockScope() {
        if (ok) WSACleanup();
    }
    bool ok = false;
};

WinsockScope& Winsock() {
    static WinsockScope scope;
    return scope;
}

// Resolves a hostname or literal to one IPv4 address.
//
// IPv4 only, deliberately: Icmp6SendEcho2 needs a bound source address and a
// different reply layout, which is a meaningful amount of extra code for a
// glanceable indicator. An IPv6-only target reports as unreachable rather than
// silently doing nothing; see the README's Limitations section.
bool ResolveIPv4(const std::wstring& host, IPAddr* out) {
    if (!out) return false;
    if (!Winsock().ok) return false;

    ADDRINFOW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;

    PADDRINFOW results = nullptr;
    if (GetAddrInfoW(host.c_str(), nullptr, &hints, &results) != 0 || !results) {
        return false;
    }

    bool found = false;
    for (PADDRINFOW it = results; it != nullptr; it = it->ai_next) {
        if (it->ai_family == AF_INET && it->ai_addr) {
            const sockaddr_in* address = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
            *out = address->sin_addr.S_un.S_addr;
            found = true;
            break;
        }
    }

    FreeAddrInfoW(results);
    return found;
}

// Milliseconds we are willing to wait for a reply.
//
// Capped at the interval so the cadence never slips: a packet that has not
// answered by the time the next one is due is a lost packet, which is the same
// rule `ping -O` applies on macOS. Floored at 300 ms so very fast intervals
// still allow a real reply, and ceilinged at 4 s so a long interval does not
// leave the grid frozen.
DWORD TimeoutFor(double interval) {
    const double milliseconds = interval * 1000.0;
    return static_cast<DWORD>(std::clamp(milliseconds, 300.0, 4000.0));
}

}  // namespace

// ------------------------------------------------------------------ lifetime

PingSession::PingSession(HWND window, unsigned monitorIndex)
    : window_(window), monitorIndex_(static_cast<LONG>(monitorIndex)) {}

PingSession::~PingSession() { Stop(); }

void PingSession::SetMonitorIndex(unsigned index) {
    InterlockedExchange(&monitorIndex_, static_cast<LONG>(index));
}

void PingSession::Start(const std::wstring& host, double interval) {
    Stop();

    // If Stop() gave up on a wedged worker, that worker is still reading the
    // fields below and still holds `this`. Starting a second thread would race
    // on all of them, so refuse. This session is finished; the monitor keeps
    // showing its last grid rather than corrupting memory.
    if (abandoned_) return;

    host_ = host;
    interval_ = std::clamp(interval, defaults::kMinInterval, defaults::kMaxInterval);
    session_ = static_cast<unsigned>(InterlockedIncrement(&g_nextSessionId));

    // Manual reset: once we ask the worker to stop it must stay stopped.
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) return;

    // 64 KB stack: this thread calls two Win32 functions in a loop and never
    // recurses. The default 1 MB reservation across eight monitors would be
    // 8 MB of address space for no reason.
    thread_.reset(CreateThread(nullptr, 64 * 1024, &PingSession::ThreadMain, this,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));
}

void PingSession::Stop() {
    if (abandoned_) return;   // nothing left here that is safe to touch

    if (stopEvent_) SetEvent(stopEvent_.get());

    if (thread_) {
        // The worker only ever waits on stopEvent_ with a bounded timeout, so
        // this normally returns at once. The generous limit is a backstop
        // against a wedged IcmpSendEcho rather than an expected wait.
        const DWORD outcome = WaitForSingleObject(thread_.get(), 10000);

        if (outcome != WAIT_OBJECT_0) {
            // The worker is still running and still holds `this` and the stop
            // event. Closing either now would hand it a recycled handle, and
            // letting the destructor run would free state it is about to read.
            //
            // So both handles are deliberately leaked and the session is marked
            // abandoned, which stops Start() from ever launching a second
            // worker on this object. One wedged thread costs 64 KB of stack; a
            // use-after-free costs the process.
            //
            // (release() must not be relied on to keep `thread_` set — it nulls
            // the member by design. Hence the explicit flag.)
            abandoned_ = true;
            (void)thread_.release();
            (void)stopEvent_.release();
            return;
        }

        thread_.reset();
    }

    stopEvent_.reset();
}

DWORD WINAPI PingSession::ThreadMain(LPVOID parameter) {
    static_cast<PingSession*>(parameter)->Run();
    return 0;
}

// ---------------------------------------------------------------- the worker

void PingSession::Run() {
    const std::wstring host = host_;
    const double interval = interval_;
    const unsigned session = session_;
    const HANDLE stop = stopEvent_.get();
    const DWORD timeout = TimeoutFor(interval);
    const DWORD intervalMs = static_cast<DWORD>(interval * 1000.0);

    // One ICMP handle for the whole session rather than one per packet.
    ScopedIcmpHandle icmp(IcmpCreateFile());

    // Resolved once per session, as `ping` does. A host that moves is picked up
    // the next time the session restarts, which the menu does on any change.
    IPAddr address = 0;
    const bool resolved = ResolveIPv4(host, &address);

    // 32 bytes out, matching Windows ping.exe so latencies are comparable.
    char payload[defaults::kPayloadBytes];
    memset(payload, 'a', sizeof(payload));

    // The reply buffer must hold ICMP_ECHO_REPLY plus the echoed payload, and
    // iphlpapi wants headroom for an optional IO_STATUS_BLOCK on top.
    std::vector<char> reply(sizeof(ICMP_ECHO_REPLY) + sizeof(payload) + 8);

    unsigned sequence = 0;

    for (;;) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;

        const ULONGLONG startedAt = GetTickCount64();

        PingResult result;
        result.sequence = sequence++;
        result.session = session;

        if (!resolved || !icmp) {
            // An unresolvable name is a real failure, and reporting it keeps the
            // grid honest rather than leaving it frozen on the last good value.
            result.reachable = false;
        } else {
            const DWORD count =
                IcmpSendEcho(icmp.get(), address, payload,
                             static_cast<WORD>(sizeof(payload)), nullptr,
                             reply.data(), static_cast<DWORD>(reply.size()), timeout);

            if (count > 0) {
                const ICMP_ECHO_REPLY* echo =
                    reinterpret_cast<const ICMP_ECHO_REPLY*>(reply.data());

                if (echo->Status == IP_SUCCESS) {
                    result.reachable = true;
                    result.hasLatency = true;
                    result.milliseconds = static_cast<double>(echo->RoundTripTime);
                } else {
                    // TTL expiry, host unreachable, admin prohibited and friends.
                    // All of them mean this packet did not come back.
                    result.reachable = false;
                }
            } else {
                result.reachable = false;   // timed out
            }
        }

        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return;

        // The window owns the result from here; it deletes it after handling.
        // PostMessage rather than SendMessage so a busy UI thread never blocks
        // the ping cadence.
        // Read the index fresh each time: a monitor removed while we were
        // sleeping renumbers the survivors, and a cached copy would route this
        // result to the wrong grid.
        const LONG currentIndex = InterlockedCompareExchange(&monitorIndex_, 0, 0);

        PingResult* payloadResult = new PingResult(result);
        if (!PostMessageW(window_, WM_PINGER_RESULT,
                          static_cast<WPARAM>(currentIndex),
                          reinterpret_cast<LPARAM>(payloadResult))) {
            // The window has gone; nothing will free this, so we must.
            delete payloadResult;
            return;
        }

        // Sleep whatever is left of the interval. IcmpSendEcho already consumed
        // some of it waiting for the reply, so the cadence stays even instead of
        // drifting by the round trip time on every packet.
        const ULONGLONG elapsed = GetTickCount64() - startedAt;
        const DWORD remaining =
            elapsed >= intervalMs ? 0 : static_cast<DWORD>(intervalMs - elapsed);

        if (remaining > 0) {
            if (WaitForSingleObject(stop, remaining) == WAIT_OBJECT_0) return;
        }
    }
}

}  // namespace pinger
