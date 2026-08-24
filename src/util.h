// Small helpers shared by the rest of the program: a minimal COM smart pointer,
// RAII wrappers for the handful of Win32 things we allocate, and UTF-8 logging.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>

#include <cstdio>
#include <string>

namespace ma {

// ---------------------------------------------------------------------------
// COM pointer. We only need construct / assign / release, so rolling our own
// keeps the project free of ATL and WRL differences between compilers.
// ---------------------------------------------------------------------------
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr& other) : p_(other.p_) {
        if (p_) p_->AddRef();
    }
    ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            reset();
            p_ = other.p_;
            if (p_) p_->AddRef();
        }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // For out-parameters: releases whatever we held first.
    T** put() {
        reset();
        return &p_;
    }
    void** put_void() {
        reset();
        return reinterpret_cast<void**>(&p_);
    }

    // Takes ownership of a pointer that already carries a reference.
    void attach(T* p) {
        reset();
        p_ = p;
    }

    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

// Owns memory handed back by COM (WAVEFORMATEX*, device id strings, ...).
template <class T>
class CoMem {
public:
    CoMem() = default;
    CoMem(const CoMem&) = delete;
    CoMem& operator=(const CoMem&) = delete;
    CoMem(CoMem&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
    ~CoMem() { reset(); }

    T* get() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    void reset() {
        if (p_) {
            CoTaskMemFree(p_);
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { reset(); }

    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr && h_ != INVALID_HANDLE_VALUE; }
    void attach(HANDLE h) {
        reset();
        h_ = h;
    }
    void reset() {
        if (h_ && h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
        }
        h_ = nullptr;
    }

private:
    HANDLE h_ = nullptr;
};

// Initializes COM for the calling thread and uninitializes it on scope exit.
class ComApartment {
public:
    ComApartment() { hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComApartment() {
        if (SUCCEEDED(hr_)) CoUninitialize();
    }
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_ = E_FAIL;
};

// ---------------------------------------------------------------------------
// Strings and logging. Device names come back as UTF-16; the console is put
// into UTF-8 mode by main() so names with non-ASCII characters survive.
// ---------------------------------------------------------------------------
std::string Utf8(const std::wstring& text);
std::wstring Wide(const std::string& text);

// Case-insensitive "does haystack contain needle".
bool ContainsNoCase(const std::wstring& haystack, const std::wstring& needle);

// Human-readable HRESULT, including the WASAPI-specific codes we can hit.
std::string HrText(HRESULT hr);

extern bool g_verbose;

void LogInfo(const char* fmt, ...);
void LogVerbose(const char* fmt, ...);
void LogError(const char* fmt, ...);

}  // namespace ma
