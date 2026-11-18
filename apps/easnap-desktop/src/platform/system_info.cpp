// Easnap - Windows version and settings queries.
#include "platform/system_info.h"

#include <initializer_list>

namespace easnap::platform {

bool WheelFollowsFocus() noexcept {
    // SPI_GETMOUSEWHEELROUTING: 0 focus, 1 hybrid, 2 the window under the pointer.
    constexpr UINT kGetMouseWheelRouting = 0x201C;
    DWORD routing = 2;
    return ::SystemParametersInfoW(kGetMouseWheelRouting, 0, &routing, 0) != FALSE && routing == 0;
}

bool SystemUsesDarkApps() noexcept {
    DWORD light = 1;
    DWORD size = sizeof(light);
    const LSTATUS status = ::RegGetValueW(
        HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
        RRF_RT_REG_DWORD, nullptr, &light, &size);
    return status == ERROR_SUCCESS && light == 0;
}

bool MediaFoundationAvailable() noexcept {
    // The libraries stay loaded once found; the delay-loaded imports of the
    // recorder then resolve against them.
    static const bool available = [] {
        for (const wchar_t* library : {L"mfplat.dll", L"mfreadwrite.dll"}) {
            if (::LoadLibraryExW(library, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) == nullptr) { return false; }
        }
        return true;
    }();
    return available;
}

WindowsVersion CurrentWindowsVersion() noexcept {
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto query = reinterpret_cast<VersionFunction>(
        reinterpret_cast<void*>(::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (query == nullptr || query(&info) != 0) {
        return {};
    }
    return {info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber};
}

}  // namespace easnap::platform
