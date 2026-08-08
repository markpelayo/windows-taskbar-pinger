// raii.h — scoped wrappers for the handle types this app touches.
//
// This is the whole of our "dependency story": about eighty lines that give the
// same guarantee Microsoft's WIL would, without adding a package manager to a
// project whose pitch is that it has none.
//
// Why it matters here specifically: the widget redraws once a second and is
// meant to be left running for weeks. A process is capped at 10,000 GDI handles
// by default, so a single leaked brush per redraw kills the app in under three
// hours. Every GDI object below frees itself on scope exit, on every path,
// including exceptions.

#pragma once

#include <windows.h>

#include <utility>

namespace pinger {

// ---------------------------------------------------------------- GDI objects

// Owns any handle freed by DeleteObject: HBRUSH, HBITMAP, HPEN, HFONT, HRGN.
template <typename T>
class GdiObject {
public:
    GdiObject() = default;
    explicit GdiObject(T handle) : handle_(handle) {}

    ~GdiObject() { reset(); }

    // Non-copyable: two owners would mean a double DeleteObject.
    GdiObject(const GdiObject&) = delete;
    GdiObject& operator=(const GdiObject&) = delete;

    GdiObject(GdiObject&& other) noexcept : handle_(other.release()) {}

    GdiObject& operator=(GdiObject&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    T get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    // Hands ownership to the caller.
    T release() {
        T handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(T handle = nullptr) {
        if (handle_ && handle_ != handle) {
            DeleteObject(static_cast<HGDIOBJ>(handle_));
        }
        handle_ = handle;
    }

private:
    T handle_ = nullptr;
};

using ScopedBrush  = GdiObject<HBRUSH>;
using ScopedBitmap = GdiObject<HBITMAP>;
using ScopedPen    = GdiObject<HPEN>;
using ScopedFont   = GdiObject<HFONT>;

// ------------------------------------------------------------ device contexts

// A memory DC from CreateCompatibleDC, released with DeleteDC.
class ScopedMemoryDC {
public:
    ScopedMemoryDC() = default;
    explicit ScopedMemoryDC(HDC dc) : dc_(dc) {}

    ~ScopedMemoryDC() { reset(); }

    ScopedMemoryDC(const ScopedMemoryDC&) = delete;
    ScopedMemoryDC& operator=(const ScopedMemoryDC&) = delete;

    ScopedMemoryDC(ScopedMemoryDC&& other) noexcept : dc_(other.release()) {}

    ScopedMemoryDC& operator=(ScopedMemoryDC&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HDC get() const { return dc_; }
    explicit operator bool() const { return dc_ != nullptr; }

    HDC release() {
        HDC dc = dc_;
        dc_ = nullptr;
        return dc;
    }

    void reset(HDC dc = nullptr) {
        if (dc_ && dc_ != dc) DeleteDC(dc_);
        dc_ = dc;
    }

private:
    HDC dc_ = nullptr;
};

// Restores whatever object was selected into a DC when the scope ends.
// SelectObject returns the previous object; forgetting to put it back is the
// other classic way to leak GDI memory.
class SelectGuard {
public:
    SelectGuard(HDC dc, HGDIOBJ object) : dc_(dc) {
        previous_ = SelectObject(dc, object);
    }

    ~SelectGuard() {
        if (dc_ && previous_) SelectObject(dc_, previous_);
    }

    SelectGuard(const SelectGuard&) = delete;
    SelectGuard& operator=(const SelectGuard&) = delete;

private:
    HDC     dc_ = nullptr;
    HGDIOBJ previous_ = nullptr;
};

// ------------------------------------------------------------ kernel handles

// Owns anything closed with CloseHandle: events, threads, mutexes.
//
// Not for IcmpCreateFile handles — those want IcmpCloseHandle, and ping.cpp has
// its own wrapper for them. They happen to be real file handles today, so
// CloseHandle appears to work, but that is undocumented.
class ScopedHandle {
public:
    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE handle) : handle_(Normalise(handle)) {}

    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : handle_(other.release()) {}

    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HANDLE get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

    HANDLE release() {
        HANDLE handle = handle_;
        handle_ = nullptr;
        return handle;
    }

    void reset(HANDLE handle = nullptr) {
        HANDLE normalised = Normalise(handle);
        if (handle_ && handle_ != normalised) CloseHandle(handle_);
        handle_ = normalised;
    }

private:
    // Several Win32 calls return INVALID_HANDLE_VALUE rather than null on
    // failure; collapsing both to null keeps every check one comparison.
    static HANDLE Normalise(HANDLE handle) {
        return handle == INVALID_HANDLE_VALUE ? nullptr : handle;
    }

    HANDLE handle_ = nullptr;
};

// ------------------------------------------------------------------- windows

// Frees a menu built with CreatePopupMenu. Submenus attached with
// MF_POPUP are destroyed with their parent, so only the root needs one.
class ScopedMenu {
public:
    ScopedMenu() = default;
    explicit ScopedMenu(HMENU menu) : menu_(menu) {}

    ~ScopedMenu() { reset(); }

    ScopedMenu(const ScopedMenu&) = delete;
    ScopedMenu& operator=(const ScopedMenu&) = delete;

    ScopedMenu(ScopedMenu&& other) noexcept : menu_(other.release()) {}

    ScopedMenu& operator=(ScopedMenu&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }

    HMENU get() const { return menu_; }
    explicit operator bool() const { return menu_ != nullptr; }

    HMENU release() {
        HMENU menu = menu_;
        menu_ = nullptr;
        return menu;
    }

    void reset(HMENU menu = nullptr) {
        if (menu_ && menu_ != menu) DestroyMenu(menu_);
        menu_ = menu;
    }

private:
    HMENU menu_ = nullptr;
};

// Releases a DC obtained with GetDC (not CreateCompatibleDC — that is
// ScopedMemoryDC above, which needs DeleteDC instead).
class ScopedWindowDC {
public:
    ScopedWindowDC(HWND window, HDC dc) : window_(window), dc_(dc) {}

    ~ScopedWindowDC() {
        if (dc_) ReleaseDC(window_, dc_);
    }

    ScopedWindowDC(const ScopedWindowDC&) = delete;
    ScopedWindowDC& operator=(const ScopedWindowDC&) = delete;

    HDC get() const { return dc_; }
    explicit operator bool() const { return dc_ != nullptr; }

private:
    HWND window_ = nullptr;
    HDC  dc_ = nullptr;
};

// A CRITICAL_SECTION that initialises and deletes itself, plus a scoped lock.
class Lock {
public:
    Lock() { InitializeCriticalSection(&cs_); }
    ~Lock() { DeleteCriticalSection(&cs_); }

    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

    void Enter() { EnterCriticalSection(&cs_); }
    void Leave() { LeaveCriticalSection(&cs_); }

private:
    CRITICAL_SECTION cs_{};
};

class LockGuard {
public:
    explicit LockGuard(Lock& lock) : lock_(lock) { lock_.Enter(); }
    ~LockGuard() { lock_.Leave(); }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Lock& lock_;
};

}  // namespace pinger
