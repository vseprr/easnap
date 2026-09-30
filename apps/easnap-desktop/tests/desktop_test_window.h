#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include "native_test_monitor.h"

namespace easnap::test {

// Actual desktop presentation, without a taskbar button or foreground/input
// activation. This is test-only chrome; the real preview/helper still paints.
class DesktopTestWindow final {
public:
    static HWND Create() {
        WNDCLASSW wc{};
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpfnWndProc = Procedure;
        wc.lpszClassName = L"EasnapDesktopVisualTest";
        wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        if (!::RegisterClassW(&wc) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
        const RECT placement=OnTestMonitor({120,120,440,312});
        return ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST,
            wc.lpszClassName, L"Easnap visual test", WS_POPUP | WS_CLIPCHILDREN,
            placement.left, placement.top, 320, 192, nullptr, nullptr, wc.hInstance, nullptr);
    }

    static bool Present(HWND window) {
        RECT bounds{};
        if (!::GetWindowRect(window, &bounds)) return false;
        MONITORINFO monitor{sizeof(monitor)};
        if (!::GetMonitorInfoW(::MonitorFromRect(&bounds, MONITOR_DEFAULTTOPRIMARY), &monitor)) return false;
        const int width = (std::min)(bounds.right - bounds.left, monitor.rcWork.right - monitor.rcWork.left);
        const int height = (std::min)(bounds.bottom - bounds.top, monitor.rcWork.bottom - monitor.rcWork.top);
        const int x = (std::clamp)(bounds.left, monitor.rcWork.left, monitor.rcWork.right - width);
        const int y = (std::clamp)(bounds.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
        const bool unchanged=x==bounds.left && y==bounds.top &&
            width==bounds.right-bounds.left && height==bounds.bottom-bounds.top;
        const HWND hit=::WindowFromPoint({x+width/2,y+height/2});
        if (unchanged && ::IsWindowVisible(window) && hit &&
            ::GetAncestor(hit,GA_ROOT)==window) return true;
        return ::SetWindowPos(window, HWND_TOPMOST, x, y, width, height,
                              SWP_NOACTIVATE | SWP_SHOWWINDOW |
                              (unchanged ? SWP_NOMOVE | SWP_NOSIZE : 0)) != FALSE;
    }

    static uint64_t display_generation() noexcept { return display_generation_; }

private:
    static LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_CREATE) ::SetTimer(window, 1, 100, nullptr);
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATEANDEAT;
        if (message == WM_WINDOWPOSCHANGING) {
            auto* position = reinterpret_cast<WINDOWPOS*>(lparam);
            if (!(position->flags & SWP_NOZORDER)) position->hwndInsertAfter = HWND_TOPMOST;
        }
        if (message == WM_DISPLAYCHANGE || message == WM_DPICHANGED) {
            ++display_generation_;
            if (::IsWindowVisible(window)) Present(window);
            return 0;
        }
        if (message == WM_TIMER && wparam == 1) {
            if (::IsWindowVisible(window)) Present(window);
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
    inline static uint64_t display_generation_{0};
};

} // namespace easnap::test
