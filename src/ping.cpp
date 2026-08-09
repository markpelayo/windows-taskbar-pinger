// ping.cpp — ICMP echo via iphlpapi, one worker thread per monitor.

#include "ping.h"

#include <winsock2.h>
#include <ws2tcpip.h>
// iphlpapi.h and icmpapi.h must follow the winsock headers.
#include <iphlpapi.h>
#include <icmpapi.h>

#include <algorithm>
#include <cstring>

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

    void reset(HANDLE handle = nullptr) {
        if (handle_ && handle_ != handle) IcmpCloseHandle(handle_);
        handle_ = (handle == INVALID_HANDLE_VALUE) ? nullptr : handle;
    }

private:
    HANDLE handle_ = nullptr;
};

// Winsock is initialised once for the process. getaddrinfo needs it; the ICMP
// APIs do not, but resolution and pinging always travel together here.
//
// Note this is first constructed on a *worker* thread, so the thread-safe
// static initialisation guard is doing real work — do not build with
// /Zc:threadSafeInit-.
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
// Never longer than the interval itself: a packet that has not answered by the
// time the next one is due is a lost packet, which is the same rule `ping -O`
// applies on macOS, and it keeps the cadence from slipping. Floored at 100 ms so
// even the fastest interval allows a real LAN reply, and capped at 1500 ms so a
// long interval cannot leave Stop() waiting on a single packet — see Stop().
DWORD TimeoutFor(double interval) {
    const double milliseconds = interval * 1000.0;
    const double capped = std::min(milliseconds, 1500.0);
    return static_cast<DWORD>(std::max(capped, 100.0));
}

}  // namespace

// ------------------------------------------------------------------ lifetime

PingSession::PingSession(HWND window, unsigned monitorIndex)
    : window_(window), monitorIndex_(monitorIndex) {}

PingSession::~PingSession() { Stop(); }

void PingSession::SetMonitorIndex(unsigned index) {
    monitorIndex_ = index;
    if (shared_) InterlockedExchange(&shared_->monitorIndex, static_cast<LONG>(index));
}

void PingSession::Start(const std::wstring& host, double interval) {
    Stop();

    auto shared = std::make_shared<PingShared>();
    shared->window = window_;
    shared->monitorIndex = static_cast<LONG>(monitorIndex_);
    shared->host = host;
    shared->interval = std::clamp(interval, defaults::kMinInterval, defaults::kMaxInterval);
    shared->session = static_cast<unsigned>(InterlockedIncrement(&g_nextSessionId));

    // Manual reset: once we ask the worker to stop it must stay stopped.
    shared->stop.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!shared->stop) return;

    // The thread gets its own shared_ptr, handed over as a raw heap pointer and
    // adopted in ThreadMain. That co-ownership is what makes an abandoned
    // worker safe.
    auto* parameter = new std::shared_ptr<PingShared>(shared);

    // 64 KB stack: this thread calls a handful of Win32 functions in a loop and
    // never recurses. STACK_SIZE_PARAM_IS_A_RESERVATION matters — without it
    // the size would be the initial commit and the reservation would silently
    // be the 1 MB from the PE header.
    thread_.reset(CreateThread(nullptr, 64 * 1024, &PingSession::ThreadMain, parameter,
                               STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr));

    if (!thread_) {
        delete parameter;
        return;
    }

    shared_ = std::move(shared);
}

void PingSession::Stop() {
    if (shared_ && shared_->stop) SetEvent(shared_->stop.get());

    if (thread_) {
        // The worker checks the stop event between packets and its ICMP wait is
        // capped at 1.5 s by TimeoutFor, so this normally returns at once and
        // never blocks the UI thread for long.
        //
        // If it does time out we simply walk away. The worker co-owns the shared
        // block, so it keeps its own stop event alive and exits on the next
        // iteration into memory that is still valid — which is why there is no
        // longer an "abandoned" flag disabling the session forever.
        //
        // Closing the thread handle here is safe even while the thread runs: it
        // only drops our reference, and nothing the worker touches lives in this
        // object any more.
        WaitForSingleObject(thread_.get(), 3000);
        thread_.reset();
    }

    shared_.reset();
}

DWORD WINAPI PingSession::ThreadMain(LPVOID parameter) {
    // Adopt the shared_ptr the caller heap-allocated for us, so the block stays
    // alive for exactly as long as this function runs.
    std::unique_ptr<std::shared_ptr<PingShared>> owned(
        static_cast<std::shared_ptr<PingShared>*>(parameter));

    if (owned && *owned) Run(*owned);
    return 0;
}

// ---------------------------------------------------------------- the worker

void PingSession::Run(const std::shared_ptr<PingShared>& shared) {
    const HWND window = shared->window;
    const HANDLE stop = shared->stop.get();
    const double interval = shared->interval;
    const unsigned session = shared->session;
    const DWORD timeout = TimeoutFor(interval);
    const DWORD intervalMs = static_cast<DWORD>(interval * 1000.0);

    // One ICMP handle for the whole session rather than one per packet.
    ScopedIcmpHandle icmp(IcmpCreateFile());

    // Resolved once per session, as `ping` does. A host that moves is picked up
    // the next time the session restarts, which the menu does on any change.
    IPAddr address = 0;
    const bool resolved = ResolveIPv4(shared->host, &address);

    // 32 bytes out, matching Windows ping.exe so latencies are comparable.
    char payload[defaults::kPayloadBytes];
    memset(payload, 'a', sizeof(payload));

    // The reply buffer must hold ICMP_ECHO_REPLY plus the echoed payload, with
    // headroom for an optional IO_STATUS_BLOCK on top. On the stack rather than
    // heap: it is under 100 bytes and this thread has 64 KB.
    char reply[sizeof(ICMP_ECHO_REPLY) + defaults::kPayloadBytes + 8];

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
                             reply, static_cast<DWORD>(sizeof(reply)), timeout);

            if (count > 0) {
                const ICMP_ECHO_REPLY* echo =
                    reinterpret_cast<const ICMP_ECHO_REPLY*>(reply);

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

        // Read the index fresh each time: a monitor removed while we were
        // sleeping renumbers the survivors, and a cached copy would route this
        // result to the wrong grid.
        const LONG currentIndex = InterlockedCompareExchange(&shared->monitorIndex, 0, 0);

        // The window owns the result from here; it deletes it after handling.
        // PostMessage rather than SendMessage so a busy UI thread never blocks
        // the ping cadence.
        PingResult* payloadResult = new PingResult(result);
        if (!PostMessageW(window, WM_PINGER_RESULT, static_cast<WPARAM>(currentIndex),
                          reinterpret_cast<LPARAM>(payloadResult))) {
            const DWORD error = GetLastError();
            delete payloadResult;

            // A full message queue is transient — the UI thread is merely busy,
            // and the next packet will get through. Only an invalid handle means
            // the window has genuinely gone, and only then should this thread
            // retire; treating both alike froze a monitor permanently the first
            // time the queue filled.
            if (error == ERROR_INVALID_WINDOW_HANDLE) return;
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
