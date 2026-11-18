// Easnap - what the running Windows offers and how its user has set it up.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

namespace easnap::platform {

/// Major, minor and build number of Windows.
struct WindowsVersion {
    DWORD major{0};
    DWORD minor{0};
    DWORD build{0};
};
/// The version of Windows that is running, not the one an application
/// manifest makes the documented version functions report.
[[nodiscard]] WindowsVersion CurrentWindowsVersion() noexcept;

/// False where Windows has no Media Foundation: the N and KN editions until
/// the Media Feature Pack is installed. Easnap loads these libraries on first
/// use, so it starts there; recording, trimming and export are not available.
[[nodiscard]] bool MediaFoundationAvailable() noexcept;

/// True when Windows is set to dark mode for applications.
[[nodiscard]] bool SystemUsesDarkApps() noexcept;

/// True when Windows sends the mouse wheel to the focused window instead of
/// the one under the pointer ("Scroll inactive windows when I hover over
/// them" is off). A window that keeps the focus then takes every wheel turn.
[[nodiscard]] bool WheelFollowsFocus() noexcept;

}  // namespace easnap::platform
