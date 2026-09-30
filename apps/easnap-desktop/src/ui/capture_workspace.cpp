#include "ui/capture_workspace.h"
#include "ui/native_controls.h"
#include <algorithm>

namespace easnap::ui {
void CaptureWorkspace::SetMode(app::CaptureMode mode) noexcept {
    if (!can_switch_mode()) { return; }
    if (editing() && mode_ == app::CaptureMode::Screenshot && mode != mode_ &&
        !editor_feature_used_ && !automatic_attempt_consumed_) {
        quick_return_armed_ = true;
    }
    mode_ = mode;
}
void CaptureWorkspace::EditorFeatureUsed() noexcept {
    if (screenshot_visible()) { editor_feature_used_ = true; FixScreenshot(); }
}
bool CaptureWorkspace::DocumentCommitted() noexcept {
    EditorFeatureUsed();
    return ConsumeQuickReturn();
}
bool CaptureWorkspace::ConsumeQuickReturn() noexcept {
    if (!screenshot_visible() || !quick_return_armed_ || automatic_attempt_consumed_) { return false; }
    quick_return_armed_ = false;
    automatic_attempt_consumed_ = true;
    return true;
}
WorkspacePointerMetrics WorkspacePointerMetrics::Native() noexcept {
    return {::GetSystemMetrics(SM_CXDRAG), ::GetSystemMetrics(SM_CYDRAG),
        ::GetSystemMetrics(SM_CXDOUBLECLK), ::GetSystemMetrics(SM_CYDOUBLECLK), ::GetDoubleClickTime()};
}
namespace {
bool InsideThreshold(POINT point, POINT origin, LONG width, LONG height) noexcept {
    const RECT box{origin.x - width / 2, origin.y - height / 2,
                   origin.x + (width + 1) / 2, origin.y + (height + 1) / 2};
    return ::PtInRect(&box, point) != FALSE;
}
LRESULT CALLBACK NoticeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if(message==WM_ERASEBKGND) { return 1; }
    if (message == theme::kChangedMessage) { ::InvalidateRect(hwnd, nullptr, FALSE); return 0; }
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(hwnd, &paint);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        PaintBuffer buffer(dc,client); dc=buffer.dc();
        theme::FillSolid(dc, client, theme::Current().panel);
        RECT placement{};
        ::GetWindowRect(hwnd, &placement);
        HFONT font = theme::CreateUiFont(15.0f, platform::MonitorForRect(placement).scale(), true);
        wchar_t text[256]{}; ::GetWindowTextW(hwnd,text,256); ::InflateRect(&client,-12,-8);
        theme::DrawLabel(dc, client, text, ::GetWindowLongPtrW(hwnd,GWLP_USERDATA) ? theme::Current().text:theme::Current().success, font,
                         DT_WORDBREAK | DT_CENTER | DT_NOPREFIX);
        ::DeleteObject(font);
        buffer.Present();
        ::EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_TIMER) { ::DestroyWindow(hwnd); return 0; }
    if (message == WM_NCHITTEST) { return HTTRANSPARENT; }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace
void WorkspacePointerSequence::Down(POINT point, DWORD time, bool native_double_click,
    WorkspacePointerTarget target, WorkspacePointerMetrics metrics) noexcept {
    second_click_ = native_double_click && previous_click_ && target == previous_target_ &&
        time - previous_time_ <= metrics.double_time &&
        InsideThreshold(point, previous_origin_, metrics.double_width, metrics.double_height);
    metrics_ = metrics;
    target_ = target;
    origin_ = point;
    down_time_ = time;
    active_ = true;
    dragged_ = false;
    previous_click_ = false;
}
void WorkspacePointerSequence::Move(POINT point) noexcept {
    if (active_ && !InsideThreshold(point, origin_, metrics_.drag_width, metrics_.drag_height)) {
        dragged_ = true;
        second_click_ = false;
    }
}
bool WorkspacePointerSequence::Up(POINT point) noexcept {
    Move(point);
    const bool content_click = active_ && !dragged_ &&
        (target_ == WorkspacePointerTarget::Content || target_ == WorkspacePointerTarget::Annotation);
    const bool complete = content_click && second_click_;
    previous_click_ = content_click && !complete;
    previous_origin_ = origin_;
    previous_time_ = down_time_;
    previous_target_ = target_;
    active_ = false;
    return complete;
}
BYTE ModeToolbarOpacity(uint64_t elapsed_ms) noexcept {
    if (elapsed_ms >= kModeToolbarFadeDurationMs) { return 0; }
    const double t = static_cast<double>(elapsed_ms) / kModeToolbarFadeDurationMs;
    return static_cast<BYTE>(255.0 * (1.0 - t * t * (3.0 - 2.0 * t)) + 0.5);
}
namespace {
void ShowCaptureNotice(const RECT& region,const std::wstring& text,bool status) {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = NoticeProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EasnapCaptureCopiedNotice";
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!registered) { return; }
    const auto monitor = platform::MonitorForRect(region);
    const LONG width=std::min<LONG>(platform::RectWidth(monitor.work_area)-theme::Scaled(24,monitor.scale()),theme::Scaled(status ? 600.0f:250.0f,monitor.scale()));
    const LONG height=theme::Scaled(status ? 64.0f:44.0f,monitor.scale());
    HWND notice = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"EasnapCaptureCopiedNotice", text.c_str(), WS_POPUP,
        monitor.work_area.left + (platform::RectWidth(monitor.work_area) - width) / 2,
        monitor.work_area.bottom - height - theme::Scaled(24.0f, monitor.scale()),
        width, height, nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (notice) {
        ::SetWindowDisplayAffinity(notice, WDA_EXCLUDEFROMCAPTURE); RoundWindow(notice);
        ::SetWindowLongPtrW(notice,GWLP_USERDATA,status);
        if (::SetTimer(notice, 1, status ? 4000:1400, nullptr)) { ::ShowWindow(notice, SW_SHOWNOACTIVATE); }
        else { ::DestroyWindow(notice); }
    }
}
} // namespace
void ShowCaptureCopiedNotice(const RECT& region) { ShowCaptureNotice(region,L"Copied to clipboard",false); }
void ShowCaptureStatusNotice(const RECT& region,const std::wstring& text) { ShowCaptureNotice(region,text,true); }
} // namespace easnap::ui
