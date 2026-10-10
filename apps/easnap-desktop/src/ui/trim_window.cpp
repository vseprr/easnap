#include "ui/trim_window.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <utility>

#include "platform/platform_utils.h"
#include "ui/trim_layout.h"
#include "ui/ui_theme.h"
#include "ui/native_controls.h"
#include <UIAutomation.h>

namespace easnap::ui {
namespace {

constexpr wchar_t kWindowClassName[] = L"EasnapTrimWindow";
constexpr wchar_t kTimelineClassName[] = L"EasnapTrimTimeline";
constexpr wchar_t kVideoClassName[] = L"EasnapTrimVideo";
constexpr wchar_t kWindowTitle[] = L"Easnap - Trim recording";
static_assert(kTrimChromeHeightDip == kWindowChromeHeight);
constexpr UINT kPreviewEventMessage = WM_APP + 1;
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT kStatusTimerIntervalMs = 100;
constexpr int kIdPlayPause = 100;
constexpr int kIdSetIn = 101;
constexpr int kIdSetOut = 102;
constexpr int kIdCancelExport = 103;
constexpr int kIdCopy = 104;

constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
// Per-control buffers alone leave holes while USER32 relocates a child tree.
// Commit this window's descendant painting together during native resizing.
// Every paint/GetDC path releases its DC promptly; no CS_OWNDC class is used.
constexpr DWORD kWindowExStyle = WS_EX_CONTROLPARENT | WS_EX_COMPOSITED;

int ScaleDip(int value, float scale) noexcept {
    return (std::max)(1, static_cast<int>(std::lround(value * scale)));
}

void SetControlFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void SetControlVisible(HWND control, bool visible) {
    if (control != nullptr) {
        ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

int MeasureTextHeight(HDC dc, const std::wstring& value, int width, UINT flags) {
    RECT measured{0,0,std::max(1,width),0};
    if(dc) { ::DrawTextW(dc,value.c_str(),static_cast<int>(value.size()),&measured,
        DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX|flags); }
    return measured.bottom;
}

TrimTextMetrics MeasureTrimReadouts(HDC dc, const std::wstring& time,
                                  const std::wstring& range, int width) {
    TrimTextMetrics result{};
    TEXTMETRICW metrics{};
    if(dc && ::GetTextMetricsW(dc,&metrics)) { result.line_height=metrics.tmHeight; }
    result.time_height=MeasureTextHeight(dc,time,width,DT_RIGHT);
    result.range_height=MeasureTextHeight(dc,range,width,DT_LEFT);
    return result;
}

std::wstring FormatTime(media::MediaTime value) {
    const int64_t nonnegative = (std::max)(int64_t{0}, value.count());
    const uint64_t milliseconds = static_cast<uint64_t>(nonnegative) / 10'000;
    const uint64_t hours = milliseconds / 3'600'000;
    const uint64_t minutes = milliseconds / 60'000 % 60;
    const uint64_t seconds = milliseconds / 1'000 % 60;
    const uint64_t remainder = milliseconds % 1'000;
    wchar_t text[64]{};
    _snwprintf_s(text, _TRUNCATE, L"%02llu:%02llu:%02llu.%03llu",
                 static_cast<unsigned long long>(hours),
                 static_cast<unsigned long long>(minutes),
                 static_cast<unsigned long long>(seconds),
                 static_cast<unsigned long long>(remainder));
    return text;
}

const wchar_t* PreviewOperationText(
    media::MediaPreviewOperation operation) noexcept {
    switch (operation) {
        case media::MediaPreviewOperation::Open:
            return L"opening";
        case media::MediaPreviewOperation::Play:
            return L"starting playback";
        case media::MediaPreviewOperation::Pause:
            return L"pausing playback";
        case media::MediaPreviewOperation::Seek:
            return L"seeking";
        case media::MediaPreviewOperation::Playback:
            return L"during playback";
        default:
            return L"processing media";
    }
}

std::wstring PreviewFailureText(
    const media::MediaPreviewSnapshot& snapshot) {
    wchar_t code[16]{};
    _snwprintf_s(code, _TRUNCATE, L"0x%08lX",
                 static_cast<unsigned long>(snapshot.error));
    return L"Preview failed while " +
           std::wstring(PreviewOperationText(snapshot.failed_operation)) +
           L" (HRESULT " + code +
           L"). The finalized source recording was kept.";
}

}  // namespace

TrimWindow::TrimWindow(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

TrimWindow::~TrimWindow() { Destroy(); }

bool TrimWindow::Open(const std::wstring& source_path,
                      const RECT& source_region) {
    Destroy();
    closing_ = false;
    if (source_path.empty() || !EnsureWindowClasses()) {
        return false;
    }
    source_region_ = source_region;
    const platform::MonitorInfo monitor = platform::MonitorForRect(source_region_);
    scale_ = monitor.scale();

    const auto initial=InitialTrimWindowSize(
        platform::RectWidth(monitor.work_area),platform::RectHeight(monitor.work_area),
        scale_,platform::RectWidth(source_region_),platform::RectHeight(source_region_));
    const LONG width=initial.width,height=initial.height;
    const LONG x = monitor.work_area.left +
                   (platform::RectWidth(monitor.work_area) - width) / 2;
    const LONG y = monitor.work_area.top +
                   (platform::RectHeight(monitor.work_area) - height) / 2;

    hwnd_ = ::CreateWindowExW(
        kWindowExStyle, kWindowClassName, kWindowTitle, kWindowStyle,
        static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
        static_cast<int>(height), nullptr, nullptr, ::GetModuleHandleW(nullptr),
        this);
    if (hwnd_ == nullptr || !StyleWindowChrome(hwnd_,true) || !CreateControls()) {
        platform::LogMessage(L"CreateWindowEx(trim view) failed (error %lu)",
                             ::GetLastError());
        Destroy();
        return false;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    LayoutControls(static_cast<int>(platform::RectWidth(client)),
                   static_cast<int>(platform::RectHeight(client)));
    RebuildFonts();
    UpdateEnabledState();
    SetStatus(L"Opening preview...");

    preview_ = media::CreateMfPlayPreview();
    if (preview_ == nullptr ||
        !preview_->Open(source_path, video_, hwnd_, kPreviewEventMessage)) {
        if (preview_ != nullptr) {
            SetStatus(PreviewFailureText(preview_->snapshot()), true);
        } else {
            SetStatus(
                L"Preview could not be created. The source recording was kept.",
                true);
        }
        UpdateEnabledState();
    }

    visible_ = true;
    ::SetTimer(hwnd_, kStatusTimerId, kStatusTimerIntervalMs, nullptr);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    ::SetFocus(play_pause_);
    return true;
}

void TrimWindow::SetExporting(bool exporting, bool cancelling) {
    if (closing_) { return; }
    const bool was_exporting = exporting_;
    exporting_ = exporting;
    cancelling_ = exporting && cancelling;
    if (exporting_ && !was_exporting) {
        export_indicator_due_ms_ = ::GetTickCount64() + 250;
    } else if (!exporting_) {
        export_indicator_due_ms_ = 0;
    }
    if (exporting_ && preview_ != nullptr) {
        static_cast<void>(preview_->Pause());
    }
    if(exporting_) {
        status_message_.clear(); status_error_=false;
        SetAccessibleName(progress_,cancelling_ ? L"Cancelling export" : L"Export progress");
    }
    UpdateStatusLayout();
    UpdateEnabledState();
}

void TrimWindow::SetExportProgress(float progress) {
    if (closing_) { return; }
    const int value = static_cast<int>(
        std::lround((std::clamp)(progress, 0.0f, 1.0f) * 1000.0f));
    if (progress_ != nullptr) {
        ::SendMessageW(progress_, PBM_SETPOS, static_cast<WPARAM>(value), 0);
        if (exporting_ &&
            ::GetTickCount64() >= export_indicator_due_ms_) {
            if(!::IsWindowVisible(progress_)) { UpdateStatusLayout(); }
        }
    }
}

void TrimWindow::SetExportResult(const std::wstring& message, bool error) {
    if (closing_) { return; }
    exporting_ = false;
    cancelling_ = false;
    export_indicator_due_ms_ = 0;
    // SetStatus performs the single layout commit with the final visibility
    // and message already known; there is no intermediate empty export row.
    SetStatus(message, error);
    UpdateEnabledState();
}

void TrimWindow::SetStatus(const std::wstring& message, bool error) {
    if (closing_) { return; }
    status_error_ = error;
    status_message_ = message;
    if (status_ != nullptr) {
        ::SetWindowTextW(status_, message.c_str());
        ::SendMessageW(status_, EM_SETSEL, 0, 0);
        ::SendMessageW(status_, EM_SCROLLCARET, 0, 0);
    }
    UpdateStatusLayout();
    if (error && hwnd_ != nullptr && visible_) {
        ::MessageBeep(MB_ICONWARNING);
    }
}

void TrimWindow::Activate() {
    if (hwnd_ != nullptr && visible_ && !closing_) {
        ::ShowWindow(hwnd_, SW_SHOWNORMAL);
        ::SetForegroundWindow(hwnd_);
    }
}

void TrimWindow::RefreshLayout() {
    if (hwnd_ == nullptr || closing_) {
        return;
    }
    RECT window_rect{};
    if (::GetWindowRect(hwnd_, &window_rect) == FALSE) {
        return;
    }
    const platform::MonitorInfo monitor = platform::MonitorForRect(window_rect);
    scale_ = monitor.scale();
    RebuildFonts();
    const RECT clamped =
        platform::ClampRectToBounds(window_rect, monitor.work_area);
    ::SetWindowPos(hwnd_, nullptr, clamped.left, clamped.top,
                   static_cast<int>(platform::RectWidth(clamped)),
                   static_cast<int>(platform::RectHeight(clamped)),
                   SWP_NOZORDER | SWP_NOACTIVATE);
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    LayoutControls(static_cast<int>(platform::RectWidth(client)),
                   static_cast<int>(platform::RectHeight(client)));
    if (preview_ != nullptr) {
        preview_->UpdateVideo();
    }
}

void TrimWindow::Hide() {
    closing_ = true;
    visible_ = false;
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kStatusTimerId);
        if (::GetCapture() == timeline_) { ::ReleaseCapture(); }
        timeline_drag_ = TimelineDrag::None;
        timeline_seek_position_.reset();
        ::ShowWindow(hwnd_, SW_HIDE);
    }
}

void TrimWindow::ReleaseMedia() {
    if (preview_ != nullptr) {
        preview_->Close();
        preview_.reset();
    }
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kStatusTimerId);
    }
}

void TrimWindow::Destroy() {
    // Preview shutdown can dispatch messages while its native child retires.
    // Remove the complete view first, then release media and native controls.
    Hide();
    ReleaseMedia();
    timeline_accessibility_.reset();
    if (hwnd_ != nullptr) {
        ::KillTimer(hwnd_, kStatusTimerId);
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (ui_font_ != nullptr) {
        ::DeleteObject(ui_font_);
        ui_font_ = nullptr;
    }
    video_ = nullptr;
    time_label_ = nullptr;
    range_label_ = nullptr;
    status_ = nullptr;
    help_ = nullptr;
    timeline_ = nullptr;
    play_pause_ = nullptr;
    set_in_ = nullptr;
    set_out_ = nullptr;
    save_ = nullptr;
    copy_ = nullptr;
    cancel_export_ = nullptr;
    discard_ = nullptr;
    progress_ = nullptr;
    range_ = app::TrimRangeModel{};
    timeline_drag_ = TimelineDrag::None;
    timeline_seek_position_.reset();
    exporting_ = false;
    cancelling_ = false;
    export_indicator_due_ms_ = 0;
    status_message_.clear(); status_error_=false;
    time_text_.clear(); range_text_.clear(); layout_ = {};
    visible_ = false;
    preview_error_reported_ = false;
}

bool TrimWindow::ProcessDialogMessage(MSG& message) const {
    return hwnd_ != nullptr && visible_ && (message.hwnd == hwnd_ || ::IsChild(hwnd_, message.hwnd)) &&
           (ConsumePopupInput(message.message, message.wParam) || ActivateFocusedButton(message, hwnd_) ||
            ::IsDialogMessageW(hwnd_, &message) != FALSE);
}

LRESULT CALLBACK TrimWindow::WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                        LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrimWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self != nullptr ? self->HandleMessage(hwnd, message, wparam, lparam)
                           : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK TrimWindow::TimelineProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrimWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self != nullptr
               ? self->HandleTimelineMessage(hwnd, message, wparam, lparam)
               : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK TrimWindow::VideoProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<TrimWindow*>(
        ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self != nullptr
               ? self->HandleVideoMessage(hwnd, message, wparam, lparam)
               : ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TrimWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) {
    if (ConsumePopupInput(message, wparam)) { return 0; }
    switch (message) {
        case theme::kChangedMessage:
            if (closing_) { return 0; }
            ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0; // Preview transport and layout are deliberately untouched.
        case WM_ERASEBKGND: {
            // During native sizing Windows can expose pixels before WM_PAINT.
            // Paint the clipped backing area in the theme color immediately.
            RECT client{}; ::GetClientRect(hwnd,&client);
            theme::FillSolid(reinterpret_cast<HDC>(wparam),client,theme::Current().panel);
            return 1;
        }
        case WM_GETMINMAXINFO: {
            auto* limits=reinterpret_cast<MINMAXINFO*>(lparam);
            RECT bounds{}; ::GetWindowRect(hwnd,&bounds);
            const auto monitor=platform::MonitorForRect(bounds);
            const UINT dpi=::GetDpiForWindow(hwnd);
            const int padding=::GetSystemMetricsForDpi(SM_CXPADDEDBORDER,dpi);
            const int frame_x=::GetSystemMetricsForDpi(SM_CXSIZEFRAME,dpi)+padding;
            const int frame_y=::GetSystemMetricsForDpi(SM_CYSIZEFRAME,dpi)+padding;
            limits->ptMinTrackSize.x=std::min(ScaleDip(kTrimMinClientWidthDip,scale_)+2*frame_x,static_cast<int>(platform::RectWidth(monitor.work_area)));
            limits->ptMinTrackSize.y=std::min(ScaleDip(kTrimMinClientHeightDip,scale_)+frame_y,static_cast<int>(platform::RectHeight(monitor.work_area)));
            return 0;
        }
        case WM_PAINT: PaintNativePanel(hwnd, scale_); return 0;
        case WM_PRINTCLIENT: {
            RECT client{}; ::GetClientRect(hwnd, &client);
            theme::DrawPanel(reinterpret_cast<HDC>(wparam), client, scale_);
            return 0;
        }
        case WM_COMMAND:
            if (!closing_) { HandleCommand(LOWORD(wparam)); }
            return 0;
        case WM_CLOSE:
            if (!closing_) { RequestClose(); }
            return 0;
        case WM_TIMER:
            if (wparam == kStatusTimerId && !closing_) {
                if(exporting_ && !::IsWindowVisible(progress_) && ::GetTickCount64()>=export_indicator_due_ms_) { UpdateStatusLayout(); }
                UpdatePosition();
                if (!closing_ && callbacks_.on_status_tick) {
                    callbacks_.on_status_tick();
                }
            }
            return 0;
        case kPreviewEventMessage:
            if (!closing_) { UpdatePreviewState(); }
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && !closing_) {
                LayoutControls(static_cast<int>(LOWORD(lparam)),
                               static_cast<int>(HIWORD(lparam)));
                if (preview_ != nullptr) {
                    preview_->UpdateVideo();
                }
            }
            return 0;
        case WM_DPICHANGED: {
            if (closing_) { return 0; }
            scale_=static_cast<float>(HIWORD(wparam))/96.0f;
            RebuildFonts();
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                               static_cast<int>(platform::RectWidth(*suggested)),
                               static_cast<int>(platform::RectHeight(*suggested)),
                               SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RefreshLayout();
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            return ControlBackground(wparam, reinterpret_cast<HWND>(lparam) == status_ && status_error_
                ? theme::Current().error : theme::Current().text);
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TrimWindow::HandleTimelineMessage(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    if (closing_ && ((message>=WM_MOUSEFIRST && message<=WM_MOUSELAST) ||
        (message>=WM_KEYFIRST && message<=WM_KEYLAST) || message==WM_SETFOCUS ||
        message==WM_GETDLGCODE || message==theme::kChangedMessage)) {
        return 0;
    }
    switch (message) {
        case WM_GETOBJECT:
            if (static_cast<LONG>(lparam) == UiaRootObjectId && timeline_accessibility_) {
                return timeline_accessibility_->HandleGetObject(wparam, lparam);
            }
            break;
        case theme::kChangedMessage:
            ::InvalidateRect(hwnd, nullptr, FALSE); return 0;
        case WM_GETDLGCODE: {
            const bool backwards = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool internal_tab = backwards ? timeline_focus_ > 0 : timeline_focus_ < 2;
            return DLGC_WANTARROWS | DLGC_WANTCHARS | (internal_tab ? DLGC_WANTTAB : 0);
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            if (timeline_accessibility_) { timeline_accessibility_->NotifyChanged(); }
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            if (!range_.valid() || exporting_) {
                return 0;
            }
            ::SetFocus(hwnd);
            const int x = GET_X_LPARAM(lparam);
            const int left = TimelineTrackLeft(hwnd);
            const int width = TimelineTrackWidth(hwnd);
            const int in_x = left + static_cast<int>(
                range_.TimelineOffsetFromTime(range_.in(),
                                              static_cast<uint32_t>(width)));
            const int out_x = left + static_cast<int>(
                range_.TimelineOffsetFromTime(range_.out(),
                                              static_cast<uint32_t>(width)));
            const int tolerance = ScaleDip(12, scale_);
            if (std::abs(x - in_x) <= tolerance &&
                std::abs(x - in_x) <= std::abs(x - out_x)) {
                timeline_drag_ = TimelineDrag::In;
            } else if (std::abs(x - out_x) <= tolerance) {
                timeline_drag_ = TimelineDrag::Out;
            } else {
                timeline_drag_ = TimelineDrag::Seek;
            }
            timeline_focus_ = timeline_drag_ == TimelineDrag::In ? 0 : timeline_drag_ == TimelineDrag::Out ? 2 : 1;
            if (timeline_accessibility_) { timeline_accessibility_->NotifyChanged(); }
            timeline_seek_position_.reset();
            ::SetCapture(hwnd);
            UpdateTimelineFromPoint(x, timeline_drag_);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (::GetCapture() == hwnd && timeline_drag_ != TimelineDrag::None) {
                UpdateTimelineFromPoint(GET_X_LPARAM(lparam), timeline_drag_);
            }
            return 0;
        case WM_LBUTTONUP:
            if (::GetCapture() == hwnd) {
                UpdateTimelineFromPoint(GET_X_LPARAM(lparam), timeline_drag_);
                ::ReleaseCapture();
            }
            timeline_drag_ = TimelineDrag::None;
            timeline_seek_position_.reset();
            return 0;
        case WM_CAPTURECHANGED:
            timeline_drag_ = TimelineDrag::None;
            timeline_seek_position_.reset();
            return 0;
        case WM_KEYDOWN: {
            if (wparam == VK_TAB) {
                FocusTimeline(std::clamp(timeline_focus_ + ((::GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1), 0, 2));
                return 0;
            }
            if (!range_.valid() || exporting_) {
                return 0;
            }
            const int editing = (::GetKeyState(VK_SHIFT) & 0x8000) != 0 ? 0 :
                (::GetKeyState(VK_CONTROL) & 0x8000) != 0 ? 2 : timeline_focus_;
            media::MediaTime candidate = editing == 0 ? range_.in() : editing == 2 ? range_.out() : range_.current();
            if (wparam == VK_HOME) {
                candidate = media::MediaTime::zero();
            } else if (wparam == VK_END) {
                candidate = range_.source_duration();
            } else if (wparam == VK_LEFT || wparam == VK_RIGHT) {
                const media::MediaTime step{10'000'000};
                candidate += wparam == VK_LEFT ? -step : step;
            } else {
                break;
            }
            candidate = (std::clamp)(candidate, media::MediaTime::zero(),
                                      range_.source_duration());
            if (editing == 0) {
                if (range_.SetIn(candidate)) {
                    ReconcileRangeEdit();
                }
            } else if (editing == 2) {
                if (range_.SetOut(candidate)) {
                    ReconcileRangeEdit();
                }
            } else {
                SeekTo(candidate);
            }
            UpdateLabels();
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_PAINT:
            PaintTimeline(hwnd);
            return 0;
        case WM_PRINTCLIENT:
            PaintTimeline(hwnd, reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_ERASEBKGND:
            PaintTimeline(hwnd, reinterpret_cast<HDC>(wparam));
            return 1;
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT TrimWindow::HandleVideoMessage(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(hwnd, &paint);
            theme::FillSolid(dc,paint.rcPaint,theme::Current().panel);
            if (!closing_ && preview_ != nullptr) {
                preview_->UpdateVideo();
            }
            ::EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_SIZE:
            if (!closing_ && wparam != SIZE_MINIMIZED && preview_ != nullptr) {
                preview_->UpdateVideo();
            }
            return 0;
        case WM_ERASEBKGND: {
            RECT client{}; ::GetClientRect(hwnd,&client);
            theme::FillSolid(reinterpret_cast<HDC>(wparam),client,theme::Current().panel);
            return 1;
        }
        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool TrimWindow::EnsureWindowClasses() {
    static const bool registered = [] {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.style = 0;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        window_class.hIcon = platform::LoadAppIcon(::GetSystemMetrics(SM_CXICON));
        window_class.hIconSm =
            platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON));
        window_class.hbrBackground = nullptr; // Every exposed pixel is painted by us.
        window_class.lpszClassName = kWindowClassName;
        if (::RegisterClassExW(&window_class) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW timeline_class{};
        timeline_class.cbSize = sizeof(timeline_class);
        timeline_class.style = CS_HREDRAW | CS_VREDRAW;
        timeline_class.lpfnWndProc = TimelineProc;
        timeline_class.hInstance = ::GetModuleHandleW(nullptr);
        timeline_class.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
        timeline_class.hbrBackground = nullptr;
        timeline_class.lpszClassName = kTimelineClassName;
        if (::RegisterClassExW(&timeline_class) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        WNDCLASSEXW video_class{};
        video_class.cbSize = sizeof(video_class);
        video_class.lpfnWndProc = VideoProc;
        video_class.hInstance = ::GetModuleHandleW(nullptr);
        video_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        video_class.hbrBackground = nullptr;
        video_class.lpszClassName = kVideoClassName;
        return ::RegisterClassExW(&video_class) != 0 ||
               ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool TrimWindow::CreateControls() {
    // VideoProc dispatches through GWLP_USERDATA just like TimelineProc. The
    // generic control factory supplies no owner, leaving paint/resize handlers
    // unreachable and exposing stale pixels around the asynchronous presenter.
    video_ = ::CreateWindowExW(0,kVideoClassName,L"",
        WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
        0,0,1,1,hwnd_,nullptr,::GetModuleHandleW(nullptr),this);
    time_label_ = CreateControl(L"STATIC", L"00:00:00.000 / 00:00:00.000",
                               SS_RIGHT, -1);
    range_label_ = CreateControl(L"STATIC", L"In —   Out —", SS_LEFT, -1);
    status_ = ::CreateWindowExW(
        0, L"EDIT", L"Opening preview…",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
            ES_READONLY,
        0, 0, 1, 1, hwnd_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if(status_ && !StyleLabel(status_)) { return false; }
    timeline_ = ::CreateWindowExW(
        0, kTimelineClassName, L"Trim timeline",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, hwnd_, nullptr,
        ::GetModuleHandleW(nullptr), this);
    play_pause_ = CreateControl(L"BUTTON", L"Play",
                                WS_TABSTOP | BS_PUSHBUTTON, kIdPlayPause);
    set_in_ = CreateControl(L"BUTTON", L"Set In",
                            WS_TABSTOP | BS_PUSHBUTTON, kIdSetIn);
    set_out_ = CreateControl(L"BUTTON", L"Set Out",
                             WS_TABSTOP | BS_PUSHBUTTON, kIdSetOut);
    save_ = CreateControl(L"BUTTON", L"Save",
                          WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    copy_ = CreateControl(L"BUTTON", L"Copy",
                          WS_TABSTOP | BS_PUSHBUTTON, kIdCopy);
    cancel_export_ = CreateControl(L"BUTTON", L"Cancel export",
                                   WS_TABSTOP | BS_PUSHBUTTON,
                                   kIdCancelExport);
    discard_ = CreateControl(L"BUTTON", L"Discard",
                             WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    progress_ = CreateControl(PROGRESS_CLASSW, L"", PBS_SMOOTH, -1);
    if(progress_ && !StyleProgress(progress_)) { return false; }
    if (progress_ != nullptr) {
        ::SendMessageW(progress_, PBM_SETRANGE32, 0, 1000);
    }
    if (timeline_) {
        timeline_accessibility_ = std::make_unique<TimelineAccessibility>(timeline_,
            [this] { return AccessibleTimeline(); },
            [this](int index, double value) { return SetAccessibleTimelineValue(index, value); },
            [this](int index) { FocusTimeline(index); });
    }
    UpdateButtonStyle(play_pause_, {Icon::Play});
    UpdateButtonStyle(save_, {Icon::Save, ButtonRole::Primary});
    UpdateButtonStyle(copy_, {Icon::Copy});
    UpdateButtonStyle(discard_, {Icon::Discard});
    SetControlVisible(cancel_export_, false);
    SetControlVisible(progress_, false);
    return video_ != nullptr && time_label_ != nullptr &&
           range_label_ != nullptr && status_ != nullptr &&
           timeline_ != nullptr && play_pause_ != nullptr && set_in_ != nullptr &&
            set_out_ != nullptr && save_ != nullptr && copy_ != nullptr &&
            cancel_export_ != nullptr && discard_ != nullptr &&
            progress_ != nullptr;
}

HWND TrimWindow::CreateControl(const wchar_t* class_name, const wchar_t* text,
                               DWORD style, int identifier) const {
    HWND control = ::CreateWindowExW(
        0, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, hwnd_,
        identifier < 0
            ? nullptr
            : reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
        ::GetModuleHandleW(nullptr), nullptr);
    if (control && ::lstrcmpW(class_name, L"BUTTON") == 0 && !StyleButton(control)) {
        ::DestroyWindow(control); return nullptr;
    }
    if(control && ::lstrcmpW(class_name,L"STATIC")==0 && !StyleLabel(control)) { ::DestroyWindow(control); return nullptr; }
    return control;
}

void TrimWindow::LayoutControls(int client_width, int client_height) {
    if (hwnd_ == nullptr || closing_ || client_width <= 0 || client_height <= 0) {
        return;
    }
    const bool show_progress=exporting_ && (cancelling_ || ::GetTickCount64()>=export_indicator_due_ms_);
    const bool show_status=!status_message_.empty() && (!exporting_ || status_error_);
    const int content_width=std::max(1,client_width-2*ScaleDip(12,scale_));
    int message_height=0;
    HDC dc=::CreateCompatibleDC(nullptr);
    const auto previous=dc && ui_font_ ? ::SelectObject(dc,ui_font_) : nullptr;
    const auto measure=[&](const std::wstring& value,int width,UINT flags) {
        return MeasureTextHeight(dc,value,width,flags);
    };
    const auto text=MeasureTrimReadouts(dc,time_text_,range_text_,content_width);
    int status_width=content_width;
    if(show_status && status_) {
        const auto margins=::SendMessageW(status_,EM_GETMARGINS,0,0);
        status_width-=LOWORD(margins)+HIWORD(margins);
        message_height=measure(status_message_,status_width,DT_EDITCONTROL)+ScaleDip(4,scale_);
    }
    auto layout=CalculateTrimWindowLayout(client_width,client_height,scale_,message_height,show_progress,text);
    const bool scroll_status=show_status && message_height>layout.status.height;
    if(scroll_status) {
        // Wrapping changes when a scrollbar is needed. Re-measure with its
        // actual width, while the central layout retains the same row budget.
        message_height=measure(status_message_,status_width-::GetSystemMetricsForDpi(SM_CXVSCROLL,
            static_cast<UINT>(std::lround(scale_*96))),DT_EDITCONTROL)+ScaleDip(4,scale_);
        layout=CalculateTrimWindowLayout(client_width,client_height,scale_,message_height,show_progress,text);
    }
    if(dc) {
        if(previous) { ::SelectObject(dc,previous); }
        ::DeleteDC(dc);
    }
    if(status_) {
        const auto style=::GetWindowLongPtrW(status_,GWL_STYLE);
        const auto desired=scroll_status ? style|WS_TABSTOP : style&~WS_TABSTOP;
        if(style!=desired) { ::SetWindowLongPtrW(status_,GWL_STYLE,desired); }
        ::ShowScrollBar(status_,SB_VERT,scroll_status);
        if(!scroll_status) {
            // Preserve full visibility after a previously scrollable notice
            // becomes short enough, including after a wider native resize.
            ::SendMessageW(status_,EM_LINESCROLL,0,-::SendMessageW(status_,EM_GETFIRSTVISIBLELINE,0,0));
        }
    }
    layout_=layout;
    struct ControlLayout { HWND hwnd; TrimLayoutRect rect; bool visible; };
    const std::array<ControlLayout,13> controls{{
        {time_label_,layout.time_label,true},
        {video_,FitTrimVideo(layout.video,platform::RectWidth(source_region_),platform::RectHeight(source_region_)),true},
        {range_label_,layout.range_label,true},{timeline_,layout.timeline,true},
        {status_,layout.status,show_status},{progress_,layout.progress,show_progress},
        {play_pause_,layout.play_pause,true},{set_in_,layout.set_in,true},{set_out_,layout.set_out,true},
        {save_,layout.save_or_cancel,!exporting_},{cancel_export_,layout.save_or_cancel,exporting_},
        {copy_,layout.copy,true},{discard_,layout.discard,true}
    }};
    HDWP batch=::BeginDeferWindowPos(static_cast<int>(controls.size()));
    // Publish geometry as a batch before any child paints. Painting during
    // EndDeferWindowPos can expose the parent's backing under controls that
    // have not reached their final position yet, particularly while shrinking.
    constexpr UINT flags=SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOCOPYBITS|SWP_NOREDRAW;
    for(const auto& [control,rect,visible]:controls) {
        if(!control || !batch) { continue; }
        RECT old{}; ::GetWindowRect(control,&old); ::MapWindowPoints(nullptr,hwnd_,reinterpret_cast<POINT*>(&old),2);
        const bool was_visible=(::GetWindowLongPtrW(control,GWL_STYLE)&WS_VISIBLE)!=0;
        if(old.left==rect.x && old.top==rect.y && old.right-old.left==rect.width && old.bottom-old.top==rect.height && visible==was_visible) { continue; }
        batch=::DeferWindowPos(batch,control,nullptr,rect.x,rect.y,rect.width,rect.height,
            flags|(visible ? SWP_SHOWWINDOW:SWP_HIDEWINDOW));
    }
    if(!batch || !::EndDeferWindowPos(batch)) {
        for(const auto& [control,rect,visible]:controls) if(control) {
            ::SetWindowPos(control,nullptr,rect.x,rect.y,rect.width,rect.height,
                flags|(visible ? SWP_SHOWWINDOW:SWP_HIDEWINDOW));
        }
    }
    for(const auto& [control,rect,visible]:controls) {
        (void)rect;
        if(control && visible) { ::InvalidateRect(control,nullptr,FALSE); }
    }
    ::InvalidateRect(hwnd_,nullptr,FALSE);
}

void TrimWindow::UpdateStatusLayout() {
    if(!hwnd_) { return; }
    RECT client{}; ::GetClientRect(hwnd_,&client);
    LayoutControls(client.right,client.bottom);
}

void TrimWindow::RebuildFonts() {
    HFONT new_ui = theme::CreateUiFont(12.5f, scale_, false);
    if(new_ui==nullptr) { return; }
    for (HWND control : {time_label_, range_label_, status_, help_, play_pause_,
                          set_in_, set_out_, save_, copy_, cancel_export_,
                          discard_}) {
        SetControlFont(control, new_ui);
    }
    if (ui_font_ != nullptr) {
        ::DeleteObject(ui_font_);
    }
    ui_font_ = new_ui;
    UpdateStatusLayout();
}

void TrimWindow::UpdatePreviewState() {
    if (preview_ == nullptr || closing_) {
        return;
    }
    const media::MediaPreviewSnapshot snapshot = preview_->snapshot();
    if (snapshot.state == media::MediaPreviewState::Ready && !range_.valid()) {
        if (range_.Initialize(snapshot.duration)) {
            SetStatus(L"");
        }
    } else if (snapshot.state == media::MediaPreviewState::Failed) {
        const std::wstring failure = PreviewFailureText(snapshot);
        SetStatus(failure, true);
        if (!preview_error_reported_) {
            preview_error_reported_ = true;
            ShowErrorMessage(hwnd_, failure.c_str());
        }
    } else if (snapshot.play_requested) {
        ::SetWindowTextW(play_pause_, L"Pause");
        UpdateButtonStyle(play_pause_, {Icon::Pause});
    } else {
        ::SetWindowTextW(play_pause_, L"Play");
        UpdateButtonStyle(play_pause_, {Icon::Play});
    }
    UpdatePosition();
    UpdateEnabledState();
}

void TrimWindow::UpdatePosition() {
    if (preview_ == nullptr || !range_.valid()) {
        return;
    }
    const media::MediaPreviewSnapshot snapshot = preview_->snapshot();
    media::MediaTime current = snapshot.position;
    const app::TrimPlaybackDecision decision =
        app::PlanTrimPlaybackProgress((snapshot.play_requested ||
                                          snapshot.state == media::MediaPreviewState::Ended) &&
                                          !snapshot.position_pending, current,
                                      range_.out());
    if (decision.action != app::TrimPlaybackAction::None) {
        ApplyPlaybackDecision(decision);
        current = decision.position;
    }
    // The tick also runs while paused; repaint only when the playhead moved.
    const bool moved = range_.current() != current;
    range_.SetCurrent(current);
    UpdateLabels();
    if (moved) { ::InvalidateRect(timeline_, nullptr, FALSE); }
}

void TrimWindow::UpdateLabels() {
    if (closing_) { return; }
    if (timeline_accessibility_) { timeline_accessibility_->NotifyChanged(); }
    if (!range_.valid()) {
        return;
    }
    const std::wstring time = FormatTime(range_.current()) + L" / " +
                              FormatTime(range_.source_duration());
    const std::wstring selected = L"In " + FormatTime(range_.in()) +
                                  L"    Out " + FormatTime(range_.out()) +
                                  L"    Selected " +
                                  FormatTime(range_.out() - range_.in());
    const bool changed=range_text_!=selected || time_text_!=time;
    if(time_text_!=time) { time_text_=time; ::SetWindowTextW(time_label_,time.c_str()); }
    if(range_text_!=selected) { range_text_=selected; ::SetWindowTextW(range_label_,selected.c_str()); }
    if(changed) {
        // Measurement must not acquire/release a composited window DC: that
        // is a presentation boundary even though no pixels are drawn here.
        // Match LayoutControls' memory-DC measurement with the actual UI font.
        HDC dc=::CreateCompatibleDC(nullptr);
        const auto previous=dc && ui_font_ ? ::SelectObject(dc,ui_font_) : nullptr;
        const auto text=MeasureTrimReadouts(dc,time_text_,range_text_,layout_.time_label.width);
        if(dc) { if(previous) { ::SelectObject(dc,previous); } ::DeleteDC(dc); }
        if(std::max(ScaleDip(22,scale_),text.time_height)!=layout_.time_label.height ||
           std::max(ScaleDip(22,scale_),text.range_height)!=layout_.range_label.height) {
            UpdateStatusLayout();
        }
    }
}

void TrimWindow::UpdateEnabledState() {
    const media::MediaPreviewState preview_state =
        preview_ != nullptr ? preview_->snapshot().state
                            : media::MediaPreviewState::Failed;
    const bool ready = range_.valid() &&
                       preview_state != media::MediaPreviewState::Opening &&
                       preview_state != media::MediaPreviewState::Failed &&
                       preview_state != media::MediaPreviewState::Closed;
    ::EnableWindow(play_pause_, ready && !exporting_);
    ::EnableWindow(set_in_, ready && !exporting_);
    ::EnableWindow(set_out_, ready && !exporting_);
    ::EnableWindow(timeline_, ready && !exporting_);
    ::EnableWindow(save_, ready && !exporting_);
    ::EnableWindow(copy_, ready && !exporting_);
    ::EnableWindow(cancel_export_, exporting_ && !cancelling_);
    ::EnableWindow(discard_, !cancelling_);
    if (timeline_accessibility_) { timeline_accessibility_->NotifyChanged(); }
}

void TrimWindow::HandleCommand(int identifier) {
    switch (identifier) {
        case kIdPlayPause:
            TogglePlayback();
            break;
        case kIdSetIn:
            SetInFromCurrent();
            break;
        case kIdSetOut:
            SetOutFromCurrent();
            break;
        case IDOK:
            BeginSave();
            break;
        case kIdCopy:
            BeginCopy();
            break;
        case kIdCancelExport:
            if (exporting_ && !cancelling_ && callbacks_.on_cancel_export) {
                cancelling_ = true;
                SetExporting(true, true);
                callbacks_.on_cancel_export();
            }
            break;
        case IDCANCEL:
            RequestClose();
            break;
        default:
            break;
    }
}

void TrimWindow::TogglePlayback() {
    if (preview_ == nullptr || !range_.valid() || exporting_) {
        return;
    }
    const media::MediaPreviewSnapshot snapshot = preview_->snapshot();
    ApplyPlaybackDecision(app::PlanTrimPlaybackToggle(
        snapshot.play_requested,
        snapshot.state == media::MediaPreviewState::Ended, snapshot.position,
        range_.in(), range_.out()));
}

void TrimWindow::SeekTo(media::MediaTime position) {
    if (preview_ == nullptr || !range_.valid()) {
        return;
    }
    range_.SetCurrent(position);
    static_cast<void>(preview_->Seek(range_.current()));
    UpdateLabels();
    ::InvalidateRect(timeline_, nullptr, FALSE);
}

void TrimWindow::ApplyPlaybackDecision(
    const app::TrimPlaybackDecision& decision) {
    if (preview_ == nullptr || !range_.valid()) {
        return;
    }
    switch (decision.action) {
        case app::TrimPlaybackAction::Play:
            static_cast<void>(preview_->Play());
            break;
        case app::TrimPlaybackAction::Pause:
            static_cast<void>(preview_->Pause());
            break;
        case app::TrimPlaybackAction::Seek:
            range_.SetCurrent(decision.position);
            static_cast<void>(preview_->Seek(range_.current()));
            break;
        case app::TrimPlaybackAction::SeekThenPlay:
            range_.SetCurrent(decision.position);
            static_cast<void>(preview_->PlayFrom(range_.current()));
            break;
        case app::TrimPlaybackAction::PauseAt:
            range_.SetCurrent(decision.position);
            static_cast<void>(preview_->PauseAt(range_.current()));
            break;
        case app::TrimPlaybackAction::None:
            return;
    }
    UpdateLabels();
    ::InvalidateRect(timeline_, nullptr, FALSE);
}

void TrimWindow::ReconcileRangeEdit() {
    ApplyPlaybackDecision(app::PlanTrimRangeEdit(
        range_.current(), range_.in(), range_.out()));
}

void TrimWindow::SetInFromCurrent() {
    if (!range_.SetIn(range_.current())) {
        SetStatus(L"In must be earlier than Out.", true);
        return;
    }
    SetStatus(L"In point updated.");
    ReconcileRangeEdit();
    UpdateLabels();
    ::InvalidateRect(timeline_, nullptr, FALSE);
}

void TrimWindow::SetOutFromCurrent() {
    if (!range_.SetOut(range_.current())) {
        SetStatus(L"Out must be later than In and within the source duration.",
                  true);
        return;
    }
    SetStatus(L"Out point updated.");
    ReconcileRangeEdit();
    UpdateLabels();
    ::InvalidateRect(timeline_, nullptr, FALSE);
}

void TrimWindow::BeginSave() {
    if (!exporting_ && range_.valid() && callbacks_.on_save) {
        callbacks_.on_save(range_.in(), range_.out());
    }
}

void TrimWindow::BeginCopy() {
    if (!exporting_ && range_.valid() && callbacks_.on_copy) {
        callbacks_.on_copy(range_.in(), range_.out());
    }
}

void TrimWindow::RequestClose() {
    if (callbacks_.on_discard_requested) {
        callbacks_.on_discard_requested();
    }
}

void TrimWindow::PaintTimeline(HWND hwnd, HDC print_dc) {
    PAINTSTRUCT paint{};
    HDC dc = print_dc ? print_dc : ::BeginPaint(hwnd, &paint);
    RECT client{};
    ::GetClientRect(hwnd, &client);
    HDC destination=dc,buffer=::CreateCompatibleDC(dc);
    HBITMAP bitmap=::CreateCompatibleBitmap(dc,client.right,client.bottom); HGDIOBJ previous_bitmap=nullptr;
    if(buffer && bitmap) { previous_bitmap=::SelectObject(buffer,bitmap); dc=buffer; }
    theme::FillSolid(dc, client, theme::Current().panel);

    const int left = TimelineTrackLeft(hwnd);
    const int width = TimelineTrackWidth(hwnd);
    const int center = layout_.timeline_track_center;
    const int track_half = ScaleDip(7, scale_);
    RECT track{left, center - track_half, left + width, center + track_half};
    theme::DrawMaterial(dc,track,7,scale_,theme::Current().timeline_track,-1);

    if (range_.valid() && width > 0) {
        const int in_x = left + static_cast<int>(range_.TimelineOffsetFromTime(
                                    range_.in(), static_cast<uint32_t>(width)));
        const int out_x = left + static_cast<int>(range_.TimelineOffsetFromTime(
                                     range_.out(), static_cast<uint32_t>(width)));
        const int current_x =
            left + static_cast<int>(range_.TimelineOffsetFromTime(
                       range_.current(), static_cast<uint32_t>(width)));
        RECT selected{in_x, track.top, (std::max)(in_x + 1, out_x), track.bottom};
        theme::FillRounded(dc,selected,ScaleDip(7,scale_),theme::Current().timeline_selection);
        const int handle_half = ScaleDip(5, scale_);
        const int handle_height = ScaleDip(14, scale_);
        RECT in_handle{in_x - handle_half, center - handle_height,
                       in_x + handle_half, center + handle_height};
        RECT out_handle{out_x - handle_half, center - handle_height,
                        out_x + handle_half, center + handle_height};
        const bool focused=::GetFocus()==hwnd && !(::SendMessageW(hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS);
        theme::DrawMaterial(dc,in_handle,5,scale_,theme::Current().raised,focused && timeline_focus_==0 ? 1.35f:1.0f);
        theme::DrawMaterial(dc,out_handle,5,scale_,theme::Current().raised,focused && timeline_focus_==2 ? 1.35f:1.0f);
        const auto& palette = theme::Current();
        const LONG marker_top = center - ScaleDip(15, scale_);
        const LONG marker_bottom = center + ScaleDip(14, scale_);
        // A rounded needle and a small coral head replace the square cutout
        // and diamond. Keep a neutral edge where the needle crosses the range.
        theme::DrawSolidLine(dc, {current_x, marker_top}, {current_x, marker_bottom},
                             palette.panel, ScaleDip(5, scale_));
        theme::DrawSolidLine(dc, {current_x, marker_top}, {current_x, marker_bottom},
                             palette.playhead, ScaleDip(2, scale_));
        if (!palette.high_contrast) {
            // The halo stays above the track: it cannot wash out the range or
            // the two trim handles. It is static, including reduced-motion mode.
            for (int radius = 6; radius >= 4; --radius) {
                const float amount = (7 - radius) * 0.08f;
                const COLORREF glow = theme::Mix(palette.panel, palette.accent, amount);
                const int extent = ScaleDip(radius, scale_);
                theme::FillRounded(dc, {current_x - extent, marker_top - extent,
                    current_x + extent, marker_top + extent}, extent, glow);
            }
        }
        const int head = ScaleDip(3, scale_);
        theme::FillRoundedBordered(dc, {current_x - head, marker_top - head,
            current_x + head, marker_top + head}, head,
            palette.high_contrast ? palette.playhead : palette.accent,
            palette.playhead, ScaleDip(1, scale_));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, theme::Current().text);
        const auto previous_font = ui_font_ ? ::SelectObject(dc, ui_font_) : nullptr;
        RECT in_text{in_x - ScaleDip(18, scale_), 0,
                     in_x + ScaleDip(18, scale_), layout_.timeline_label_height};
        RECT out_text{out_x - ScaleDip(18, scale_), 0,
                      out_x + ScaleDip(18, scale_), layout_.timeline_label_height};
        ::DrawTextW(dc, L"In", 2, &in_text, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        ::DrawTextW(dc, L"Out", 3, &out_text, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
        if(previous_font) { ::SelectObject(dc,previous_font); }
    }
    if(timeline_focus_==1 && ::GetFocus()==hwnd && !(::SendMessageW(hwnd,WM_QUERYUISTATE,0,0)&UISF_HIDEFOCUS) && range_.valid()) {
        RECT thumb=AccessibleTimeline().values[timeline_focus_].bounds;
        ::MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&thumb),2);
        theme::DrawSolidLine(dc,{thumb.left,thumb.bottom},{thumb.right,thumb.bottom},theme::Current().text,ScaleDip(1,scale_));
    }
    if(previous_bitmap) { ::BitBlt(destination,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY); ::SelectObject(buffer,previous_bitmap); }
    if(bitmap) { ::DeleteObject(bitmap); } if(buffer) { ::DeleteDC(buffer); }
    if (!print_dc) { ::EndPaint(hwnd, &paint); }
}

void TrimWindow::UpdateTimelineFromPoint(int x, TimelineDrag drag) {
    if (closing_ || !range_.valid() || exporting_ || timeline_ == nullptr) {
        return;
    }
    const int left = TimelineTrackLeft(timeline_);
    const int width = TimelineTrackWidth(timeline_);
    const uint32_t offset = static_cast<uint32_t>(
        (std::clamp)(x - left, 0, width));
    const media::MediaTime value = range_.TimeFromTimelineOffset(
        offset, static_cast<uint32_t>(width));
    bool changed = false;
    if (drag == TimelineDrag::In) {
        changed = range_.SetIn(value);
    } else if (drag == TimelineDrag::Out) {
        changed = range_.SetOut(value);
    } else if (drag == TimelineDrag::Seek) {
        const bool play_requested =
            preview_ != nullptr && preview_->snapshot().play_requested;
        const auto planned = app::PlanTrimTimelineSeek(
            play_requested, value, range_.in(), range_.out());
        if (!planned.has_value()) {
            return;
        }
        // A stationary click produces down, optional same-coordinate moves,
        // and up. Playback can advance between them; comparing with current()
        // would seek backwards again on release. Deduplicate only this gesture,
        // so a later click at the same point still intentionally restarts there.
        if (timeline_seek_position_ == value) {
            return;
        }
        changed = true;
    }
    if (changed) {
        if (drag == TimelineDrag::Seek) {
            SeekTo(value);
            timeline_seek_position_ = value;
        } else {
            ReconcileRangeEdit();
        }
        UpdateLabels();
        ::InvalidateRect(timeline_, nullptr, FALSE);
    }
}

int TrimWindow::TimelineTrackLeft(HWND) const noexcept {
    return ScaleDip(18, scale_);
}

int TrimWindow::TimelineTrackWidth(HWND hwnd) const noexcept {
    RECT client{};
    if (hwnd == nullptr || ::GetClientRect(hwnd, &client) == FALSE) {
        return 1;
    }
    return (std::max)(1, static_cast<int>(platform::RectWidth(client)) -
                            ScaleDip(36, scale_));
}

}  // namespace easnap::ui

namespace easnap::ui {
AccessibleTimelineSnapshot TrimWindow::AccessibleTimeline() const {
    AccessibleTimelineSnapshot result;
    result.focus = timeline_focus_;
    result.enabled = !closing_ && range_.valid() && !exporting_ && ::IsWindowEnabled(timeline_);
    if (!timeline_ || !range_.valid()) { return result; }
    RECT client{}; ::GetClientRect(timeline_, &client);
    const int left = TimelineTrackLeft(timeline_);
    const int width = TimelineTrackWidth(timeline_);
    const auto seconds = [](media::MediaTime time) { return static_cast<double>(time.count()) / 10'000'000.0; };
    const media::MediaTime values[]{range_.in(), range_.current(), range_.out()};
    for (int i = 0; i != 3; ++i) {
        const int x = left + static_cast<int>(range_.TimelineOffsetFromTime(values[i], static_cast<uint32_t>(width)));
        RECT bounds{x-ScaleDip(12, scale_), 0, x+ScaleDip(12, scale_), client.bottom};
        ::MapWindowPoints(timeline_, nullptr, reinterpret_cast<POINT*>(&bounds), 2);
        result.values[i] = {seconds(values[i]), i == 2 ? seconds(range_.in() + media::MediaTime{1}) : 0,
            i == 0 ? seconds(range_.out() - media::MediaTime{1}) : seconds(range_.source_duration()), bounds};
    }
    return result;
}
bool TrimWindow::SetAccessibleTimelineValue(int index, double seconds) {
    if (!AccessibleTimeline().enabled || index < 0 || index > 2 || !std::isfinite(seconds)) { return false; }
    const auto bounds = AccessibleTimeline().values[index];
    if (seconds < bounds.minimum || seconds > bounds.maximum) { return false; }
    const auto ticks = static_cast<int64_t>(std::round(static_cast<long double>(seconds) * 10'000'000.0L));
    const media::MediaTime value{ticks};
    if (index == 1) {
        if (!app::PlanTrimTimelineSeek(preview_->snapshot().play_requested,
                                      value, range_.in(), range_.out())) { return false; }
        SeekTo(value);
    }
    else {
        const bool changed = index == 0 ? range_.SetIn(value) : range_.SetOut(value);
        if (!changed) { return value == (index == 0 ? range_.in() : range_.out()); }
        ReconcileRangeEdit();
    }
    UpdateLabels(); ::InvalidateRect(timeline_, nullptr, FALSE);
    return true;
}
void TrimWindow::FocusTimeline(int index) {
    if (closing_) { return; }
    timeline_focus_ = std::clamp(index, 0, 2);
    ::SetFocus(timeline_);
    if (timeline_accessibility_) { timeline_accessibility_->NotifyChanged(); }
    ::InvalidateRect(timeline_, nullptr, FALSE);
}
}
