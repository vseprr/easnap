// Easnap - platform core implementation.
#include "platform/platform_utils.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>

#include "resource.h"

namespace easnap::platform {

// ---------------------------------------------------------------------------
// ComRuntime
// ---------------------------------------------------------------------------

ComRuntime::ComRuntime() {
    const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    initialized_ = SUCCEEDED(hr);
    if (!initialized_) {
        LogHresult(L"CoInitializeEx", hr);
    }
}

ComRuntime::~ComRuntime() {
    if (initialized_) {
        ::CoUninitialize();
    }
}

// ---------------------------------------------------------------------------
// Process setup
// ---------------------------------------------------------------------------

bool RestrictDllSearchPath() {
    // Dynamic loads by bare name resolve from System32 (and directories added
    // explicitly), never from the directory a portable copy was unpacked into.
    // Static imports are covered by the /DEPENDENTLOADFLAG linker option.
    return ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_USER_DIRS) != FALSE;
}

// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

namespace {
std::atomic<LogSink> g_log_sink{nullptr};
}  // namespace

void SetLogSink(LogSink sink) noexcept { g_log_sink.store(sink, std::memory_order_release); }

void LogMessage(const wchar_t* format, ...) {
    wchar_t buffer[1024]{};
    va_list args;
    va_start(args, format);
    ::vswprintf(buffer, ARRAYSIZE(buffer), format, args);
    va_end(args);
    buffer[ARRAYSIZE(buffer) - 1] = L'\0';  // A line that did not fit is cut, not dropped.

    if (const LogSink sink = g_log_sink.load(std::memory_order_acquire)) { sink(buffer); }
    wchar_t line[1100]{};
    ::swprintf(line, ARRAYSIZE(line), L"[easnap] %s\n", buffer);
    ::OutputDebugStringW(line);
}

void LogHresult(const wchar_t* what, HRESULT hr) { LogMessage(L"%s failed (hr=0x%08X)", what, static_cast<unsigned>(hr)); }

namespace {
int g_modal_loops = 0;  // UI thread only.
}  // namespace

ModalLoopScope::ModalLoopScope() noexcept { ++g_modal_loops; }
ModalLoopScope::~ModalLoopScope() { --g_modal_loops; }
bool ModalLoopActive() noexcept { return g_modal_loops != 0; }

HICON LoadAppIcon(int desired_size) {
    HMODULE module = ::GetModuleHandleW(nullptr);

    // Capability detection instead of an OS version check: LoadIconWithScaleDown
    // picks the best matching size out of the multi-resolution .ico and scales it
    // with a high quality filter. LoadImageW is the documented fallback.
    using LoadIconWithScaleDownFn = HRESULT(WINAPI*)(HINSTANCE, PCWSTR, int, int, HICON*);
    if (HMODULE comctl = ::LoadLibraryExW(L"comctl32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        const auto load_scaled =
            reinterpret_cast<LoadIconWithScaleDownFn>(
                reinterpret_cast<void*>(::GetProcAddress(comctl, "LoadIconWithScaleDown")));
        if (load_scaled != nullptr) {
            HICON icon = nullptr;
            const HRESULT hr = load_scaled(module, MAKEINTRESOURCEW(IDI_EASNAP_APP), desired_size, desired_size, &icon);
            if (SUCCEEDED(hr) && icon != nullptr) {
                // comctl32 stays loaded for the process lifetime on purpose: the
                // returned icon must outlive this call.
                return icon;
            }
        }
        ::FreeLibrary(comctl);
    }

    return static_cast<HICON>(::LoadImageW(module, MAKEINTRESOURCEW(IDI_EASNAP_APP), IMAGE_ICON, desired_size,
                                           desired_size, LR_DEFAULTCOLOR));
}

}  // namespace easnap::platform
