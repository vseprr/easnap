// Easnap - window class registration and the binding of a window to its C++ owner.
//
// The owner travels as the CreateWindowExW creation parameter and is stored in
// GWLP_USERDATA on WM_NCCREATE (window implementation guide, section 1).
// Messages that arrive before WM_NCCREATE or after the owner was unbound find
// no owner; each window procedure decides what they do, usually
// DefWindowProcW. Nothing here adds class styles, extended styles or teardown:
// a window procedure that clears its binding on WM_NCDESTROY does so itself.
// Header-only, so the preview host and the tests use it without linking more.
#pragma once

#include <windows.h>

namespace easnap::platform {

/// Registers `window_class` (cbSize is filled in); true also when this process already has a class of that name.
[[nodiscard]] inline bool RegisterWindowClass(WNDCLASSEXW window_class) noexcept {
    window_class.cbSize = sizeof(window_class);
    return ::RegisterClassExW(&window_class) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

/// Stores the creation parameter as the owner on WM_NCCREATE; returns the owner bound to `window`, or nullptr.
template <typename Owner>
[[nodiscard]] Owner* BindWindowOwner(HWND window, UINT message, LPARAM lparam) noexcept {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    return reinterpret_cast<Owner*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
}

/// Clears the owner binding, so later messages of `window` find no owner.
inline void UnbindWindowOwner(HWND window) noexcept { ::SetWindowLongPtrW(window, GWLP_USERDATA, 0); }

/// Window procedure that hands every message of a bound window to `Handler` of its owner, and the
/// messages without an owner to DefWindowProcW.
template <typename Owner, LRESULT (Owner::*Handler)(HWND, UINT, WPARAM, LPARAM)>
LRESULT CALLBACK BoundWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (Owner* owner = BindWindowOwner<Owner>(window, message, lparam); owner != nullptr) {
        return (owner->*Handler)(window, message, wparam, lparam);
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace easnap::platform
