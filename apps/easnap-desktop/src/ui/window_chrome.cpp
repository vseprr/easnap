#include "ui/window_chrome.h"

#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <array>
#include <memory>

#include "platform/paint_buffer.h"
#include "platform/window_class.h"
#include "strings/strings.h"
#include "ui/control_ids.h"
#include "ui/icons.h"
#include "ui/native_controls.h"
#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {
/// The caption buttons, indexes into kCaptionButtons, in creation order.
enum CaptionButton : size_t {
    kCloseButton,
    kMinimizeButton,
    kMaximizeButton,
    kCaptionButtonCount
};
/// One row per caption button. They have no control identifier (the header
/// tells them apart by handle) and no tooltip: the caption is the accessible
/// name. Minimize and maximize exist on a resizable window only.
constexpr std::array<StyledButtonRow, kCaptionButtonCount> kCaptionButtons{{
    {0, WS_TABSTOP | BS_PUSHBUTTON, IDS_SHELL_CHROME_CLOSE, {Icon::Discard, ButtonRole::Standard, true}},
    {0, WS_TABSTOP | BS_PUSHBUTTON, IDS_SHELL_CHROME_MINIMIZE, {Icon::Minimize, ButtonRole::Standard, true}},
    {0, WS_TABSTOP | BS_PUSHBUTTON, IDS_SHELL_CHROME_MAXIMIZE, {Icon::Maximize, ButtonRole::Standard, true}},
}};
struct WindowChrome {
    HWND owner{}, header{}, close{}, minimize{}, maximize{};
    HFONT font{};
    UINT font_dpi{};
    bool resizable{};
    ~WindowChrome() {
        if (font) { ::DeleteObject(font); }
    }
    void PaintResize() const {
        // Finish only our own HWNDs before returning to the native resize
        // loop. Never synchronously paint the out-of-process video renderer.
        constexpr UINT flags = RDW_UPDATENOW | RDW_NOCHILDREN | RDW_NOERASE;
        ::RedrawWindow(owner, nullptr, nullptr, flags);
        ::EnumChildWindows(owner, [](HWND child, LPARAM) -> BOOL {
            if(::GetWindowThreadProcessId(child,nullptr)==::GetCurrentThreadId() && ::IsWindowVisible(child)) {
                ::RedrawWindow(child,nullptr,nullptr,RDW_UPDATENOW|RDW_NOCHILDREN|RDW_NOERASE);
            }
            return TRUE; }, 0);
    }
    void Layout() {
        const float scale = ::GetDpiForWindow(owner) / 96.0f;
        RECT r{};
        ::GetClientRect(owner, &r);
        const int height = theme::Scaled(kWindowChromeHeight, scale), button = theme::Scaled(theme::kCaptionButtonSize, scale), gap = theme::Scaled(theme::kCaptionButtonGap, scale);
        constexpr UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS | SWP_NOREDRAW;
        ::SetWindowPos(header, nullptr, 0, 0, r.right, height, flags);
        HDWP batch = ::BeginDeferWindowPos(3);
        LONG x = r.right - gap - button;
        for (HWND control : {close, maximize, minimize})
            if (control) {
                if (batch) { batch = ::DeferWindowPos(batch, control, nullptr, x, (height - button) / 2, button, button, flags); }
                x -= button + gap;
            }
        if (!batch || !::EndDeferWindowPos(batch)) {
            x = r.right - gap - button;
            for (HWND control : {close, maximize, minimize})
                if (control) {
                    ::SetWindowPos(control, nullptr, x, (height - button) / 2, button, button, flags);
                    x -= button + gap;
                }
        }
        const UINT dpi = ::GetDpiForWindow(owner);
        if (!font || font_dpi != dpi) {
            HFONT next = theme::CreateUiFont(theme::kBodyFontSize, scale, true);
            if (next) {
                if (font) { ::DeleteObject(font); }
                font = next;
                font_dpi = dpi;
            }
        }
        // Resizable windows use DWM corners. Replacing their region during
        // every WM_SIZE invalidates the compositor's resize backing surface.
        if (!resizable) { RoundWindow(owner); }
        ::RedrawWindow(header, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
    }
};
LRESULT CALLBACK ChromeHeaderProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* data = platform::BindWindowOwner<WindowChrome>(hwnd, message, lparam);
    if (!data) { return ::DefWindowProcW(hwnd, message, wparam, lparam); }
    switch (message) {
        case WM_NCHITTEST: {
            if (data->resizable && !::IsZoomed(data->owner)) {
                RECT r{};
                ::GetWindowRect(data->owner, &r);
                const int edge = theme::Scaled(theme::kResizeEdge, ::GetDpiForWindow(hwnd) / 96.0f);
                if (GET_X_LPARAM(lparam) < r.left + edge || GET_X_LPARAM(lparam) >= r.right - edge || GET_Y_LPARAM(lparam) < r.top + edge) { return HTTRANSPARENT; }
            }
            return HTCLIENT;
        }
        case WM_PAINT:
        case WM_PRINTCLIENT:
        case WM_ERASEBKGND: {
            PAINTSTRUCT ps{};
            HDC target = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
            RECT r{};
            ::GetClientRect(hwnd, &r);
            platform::PaintBuffer buffer(target, r);
            theme::FillSolid(buffer.dc(), r, theme::Current().panel);
            const float scale = ::GetDpiForWindow(hwnd) / 96.0f;
            wchar_t title[256]{};
            ::GetWindowTextW(data->owner, title, 256);
            r.left = theme::Scaled(theme::kCaptionTitleInset, scale);
            r.right -= theme::Scaled(data->resizable ? theme::kCaptionTitleReserveResizable : theme::kCaptionTitleReserveFixed, scale);
            theme::DrawLabel(buffer.dc(), r, title, theme::Current().muted_text, data->font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            buffer.Present();
            if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
            return message == WM_ERASEBKGND ? 1 : 0;
        }
        case WM_COMMAND:
            if (reinterpret_cast<HWND>(lparam) == data->close) {
                ::PostMessageW(data->owner, WM_CLOSE, 0, 0);
            } else if (reinterpret_cast<HWND>(lparam) == data->minimize) {
                ::SendMessageW(data->owner, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            } else if (reinterpret_cast<HWND>(lparam) == data->maximize) {
                ::SendMessageW(data->owner, WM_SYSCOMMAND, ::IsZoomed(data->owner) ? SC_RESTORE : SC_MAXIMIZE, 0);
            }
            return 0;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_RBUTTONUP: {
            POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ClientToScreen(hwnd, &p);
            ::SendMessageW(data->owner, message == WM_LBUTTONDOWN ? WM_NCLBUTTONDOWN : message == WM_LBUTTONDBLCLK ? WM_NCLBUTTONDBLCLK
                                                                                                                   : WM_NCRBUTTONUP,
                           HTCAPTION, MAKELPARAM(p.x, p.y));
            return 0;
        }
        case theme::kChangedMessage: ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN); return 0;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}
LRESULT CALLBACK ChromeParentProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
    auto* data = reinterpret_cast<WindowChrome*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, ChromeParentProc, id);
        delete data;
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_NCCALCSIZE) {
        if (data->resizable) {
            auto* rect = reinterpret_cast<RECT*>(lparam);
            const LONG top = rect->top;
            // Preserve DWM's left/right/bottom resize frame. Removing the
            // entire frame makes its old surface follow the moving origin
            // before the new child layout is presented on Windows 10.
            ::DefSubclassProc(hwnd, message, wparam, lparam);
            const UINT dpi = ::GetDpiForWindow(hwnd);
            rect->top = top + (::IsZoomed(hwnd) ? ::GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) +
                                                      ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi)
                                                : 0);
        }
        // WVR_REDRAW mispositions children when nonclient insets exist.
        // Layout invalidates and paints our owned surfaces after positioning.
        return 0;
    }
    if (message == WM_NCPAINT && !data->resizable) { return 0; }
    if (message == WM_NCACTIVATE && !data->resizable) { return TRUE; }
    // WM_NCUAHDRAWCAPTION and WM_NCUAHDRAWFRAME, undocumented: with visual
    // styles, a press on the caption makes Windows send them to paint its own
    // caption and frame, over the client area when the window is layered for
    // its capture exclusion (ADR-008). Like WM_NCPAINT, a fixed window with
    // this chrome has none to paint.
    constexpr UINT kDrawThemedCaption = 0x00AE, kDrawThemedFrame = 0x00AF;
    if ((message == kDrawThemedCaption || message == kDrawThemedFrame) && !data->resizable) { return 0; }
    if (message == WM_NCHITTEST && data->resizable && !::IsZoomed(hwnd)) {
        RECT r{};
        ::GetWindowRect(hwnd, &r);
        const int edge = theme::Scaled(theme::kResizeEdge, ::GetDpiForWindow(hwnd) / 96.0f);
        const int x = GET_X_LPARAM(lparam), y = GET_Y_LPARAM(lparam);
        const bool left = x < r.left + edge, right = x >= r.right - edge, top = y < r.top + edge, bottom = y >= r.bottom - edge;
        if (top) { return left ? HTTOPLEFT : right ? HTTOPRIGHT
                                                   : HTTOP; }
        if (bottom) { return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT
                                                         : HTBOTTOM; }
        if (left || right) { return left ? HTLEFT : HTRIGHT; }
        return HTCLIENT;
    }
    const bool resizing = message == WM_SIZE && wparam != SIZE_MINIMIZED;
    if (resizing) { data->Layout(); }
    const LRESULT result = ::DefSubclassProc(hwnd, message, wparam, lparam);
    if (message == WM_WINDOWPOSCHANGED && data->resizable &&
        !(reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_NOSIZE)) { data->PaintResize(); }
    if (message == WM_SETTEXT) { ::InvalidateRect(data->header, nullptr, FALSE); }
    return result;
}
}  // namespace
bool StyleWindowChrome(HWND window, bool resizable) {
    static const bool registered = [] {
        WNDCLASSEXW cls{};
        cls.style = CS_DBLCLKS;
        cls.lpfnWndProc = ChromeHeaderProc;
        cls.hInstance = ::GetModuleHandleW(nullptr);
        cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = L"EasnapWindowChrome";
        return platform::RegisterWindowClass(cls);
    }();
    if (!registered) { return false; }
    auto data = std::make_unique<WindowChrome>();
    data->owner = window;
    data->resizable = resizable;
    data->header = ::CreateWindowExW(WS_EX_CONTROLPARENT, L"EasnapWindowChrome", strings::Get(IDS_SHELL_CHROME_CONTROLS), WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 1, 1, window, nullptr, ::GetModuleHandleW(nullptr), data.get());
    if (!data->header) { return false; }
    data->close = CreateStyledButton(data->header, kCaptionButtons[kCloseButton], WS_VISIBLE);
    if (resizable) {
        data->minimize = CreateStyledButton(data->header, kCaptionButtons[kMinimizeButton], WS_VISIBLE);
        data->maximize = CreateStyledButton(data->header, kCaptionButtons[kMaximizeButton], WS_VISIBLE);
    }
    if (!data->close || (resizable && (!data->minimize || !data->maximize)) ||
        !::SetWindowSubclass(window, ChromeParentProc, kChromeSubclass, reinterpret_cast<DWORD_PTR>(data.get()))) {
        ::DestroyWindow(data->header);
        return false;
    }
    auto* installed = data.release();
    if (resizable) {
        // Unsupported on older Windows versions: a rectangular frame is the
        // safe fallback, without region replacement or a separate shadow HWND.
        const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
        ::DwmSetWindowAttribute(window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    }
    ::SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    installed->Layout();
    return true;
}

} // namespace easnap::ui
