// Easnap - platform core: COM and handle lifetime, process setup, logging and
// modal loops.
//
// The other Win32/COM/Direct2D details the rest of the application needs live
// beside this file, one concern each: screen_geometry, system_info,
// bgra_image, graphics_device, image_output and files. Feature code must never
// call raw Win32 APIs that are wrapped in this layer, so the Windows 11 /
// future-platform work stays confined to it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <wrl/client.h>

#include <memory>
#include <type_traits>

namespace easnap::platform {

/// COM smart pointer used throughout Easnap.
template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

// ---------------------------------------------------------------------------
// COM / lifetime helpers
// ---------------------------------------------------------------------------

/// RAII wrapper around CoInitializeEx for a single thread.
class ComRuntime {
public:
    ComRuntime();
    ~ComRuntime();

    ComRuntime(const ComRuntime&) = delete;
    ComRuntime& operator=(const ComRuntime&) = delete;

    [[nodiscard]] bool ok() const noexcept { return initialized_; }

private:
    bool initialized_{false};
};

/// Deleter that lets std::unique_ptr own GDI/USER handles.
struct WindowDeleter {
    void operator()(HWND hwnd) const noexcept {
        if (hwnd != nullptr) {
            ::DestroyWindow(hwnd);
        }
    }
};
using UniqueWindow = std::unique_ptr<std::remove_pointer_t<HWND>, WindowDeleter>;

/// Deleter for kernel objects returned as HANDLE (mutexes, files, events).
struct HandleDeleter {
    void operator()(HANDLE handle) const noexcept {
        if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle);
        }
    }
};
using UniqueHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

/// Call first in every process entry point, before any library is loaded.
/// False when Windows refused; GetLastError tells why.
[[nodiscard]] bool RestrictDllSearchPath();

// ---------------------------------------------------------------------------
// Diagnostics (privacy preserving: never log pixels, paths or typed text)
// ---------------------------------------------------------------------------

/// Writes one line to the debugger and to the log sink, if one is set. A line
/// must never contain user content: no path, file name, window title or text
/// the user typed.
void LogMessage(const wchar_t* format, ...);
/// Logs that `what` failed with `hr`.
void LogHresult(const wchar_t* what, HRESULT hr);
/// Receives every `LogMessage` line, without a line break, on the calling
/// thread. `platform/diagnostics` sets it to the log file; null stops it.
using LogSink = void (*)(const wchar_t* line) noexcept;
/// Sets the receiver of every log line; null stops it.
void SetLogSink(LogSink sink) noexcept;

/// Marks a system dialog that runs its own message loop on the UI thread.
/// While one is open, the messages of every window of the application are
/// dispatched from inside it, so nothing the dialog's caller still refers to
/// may be destroyed. Exit and session-end handling ask `ModalLoopActive`.
class ModalLoopScope final {
public:
    ModalLoopScope() noexcept;
    ~ModalLoopScope();
    ModalLoopScope(const ModalLoopScope&) = delete;
    ModalLoopScope& operator=(const ModalLoopScope&) = delete;
};
/// True while a ModalLoopScope is open.
[[nodiscard]] bool ModalLoopActive() noexcept;

/// The application icon at `desired_size` pixels.
HICON LoadAppIcon(int desired_size);

}  // namespace easnap::platform

// Transitional, removed by the next commit: the callers of this header still
// expect everything it used to declare.
#include "platform/bgra_image.h"
#include "platform/files.h"
#include "platform/graphics_device.h"
#include "platform/image_output.h"
#include "platform/screen_geometry.h"
#include "platform/system_info.h"
