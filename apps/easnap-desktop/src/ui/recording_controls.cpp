#include "ui/recording_controls.h"

#include <algorithm>
#include <array>
#include <utility>

#include "platform/platform_utils.h"
#include "ui/ui_theme.h"
#include "ui/native_controls.h"
#include <windowsx.h>
#include <commctrl.h>

namespace easnap::ui {
namespace {

constexpr wchar_t kPanelClassName[] = L"EasnapRecordingControls";
constexpr wchar_t kBoundaryClassName[] = L"EasnapRecordingBoundary";
constexpr UINT_PTR kStatusTimerId = 1;
constexpr UINT kStatusTimerIntervalMs = 250;
constexpr int kPanelWidthDip = 390;
constexpr int kActivePanelWidthDip = 310;

constexpr int kIdTitle = 100;
constexpr int kIdElapsed = 101;
constexpr int kIdStatus = 103;
constexpr int kIdFormatLabel = 104;
constexpr int kIdFormatMp4 = 105;
constexpr int kIdFormatGif = 106;
constexpr int kIdGifQualityLabel = 107;
constexpr int kIdGifHighQuality = 108;
constexpr int kIdGifLowSize = 109;
constexpr int kIdAudioLabel = 102;
constexpr int kIdAppearance = 115;
constexpr int kIdAudioSystem = 111;
constexpr int kIdAudioMicrophone = 112;
constexpr int kIdIncludeCursor = 113;
constexpr int kIdPauseResume = 120;
constexpr int kIdStop = 121;

constexpr DWORD kPanelStyle = WS_POPUP | WS_CLIPCHILDREN;
constexpr DWORD kPanelExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;

#ifndef WDA_EXCLUDEFROMCAPTURE
constexpr DWORD WDA_EXCLUDEFROMCAPTURE = 0x00000011;
#endif

[[nodiscard]] int ScaleDip(int value, float scale) noexcept {
    return (std::max)(1, static_cast<int>(static_cast<float>(value) * scale + 0.5f));
}

[[nodiscard]] LONG ClampCoordinate(LONG value, LONG minimum, LONG maximum) noexcept {
    if (maximum < minimum) {
        return minimum;
    }
    return (std::clamp)(value, minimum, maximum);
}

void AdjustPanelRect(RECT& rect, UINT dpi) {
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjust_for_dpi = []() -> AdjustForDpiFn {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 == nullptr
                   ? nullptr
                   : reinterpret_cast<AdjustForDpiFn>(reinterpret_cast<void*>(
                         ::GetProcAddress(user32, "AdjustWindowRectExForDpi")));
    }();
    if (adjust_for_dpi != nullptr &&
        adjust_for_dpi(&rect, kPanelStyle, FALSE, kPanelExStyle, dpi) != FALSE) {
        return;
    }
    ::AdjustWindowRectEx(&rect, kPanelStyle, FALSE, kPanelExStyle);
}

void SetControlVisible(HWND control, bool visible) {
    if (control != nullptr) {
        ::ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
    }
}

void SetControlFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

[[nodiscard]] bool ApplyCaptureExclusion(HWND hwnd) noexcept {
    return hwnd != nullptr && ::SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE) != FALSE;
}

}  // namespace

class RecordingControls::Window final {
public:
    explicit Window(RecordingControls& owner) : owner_(owner) {}
    ~Window() { Destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool Create() {
        if (!EnsureWindowClasses()) {
            return false;
        }

        panel_ = ::CreateWindowExW(
            kPanelExStyle, kPanelClassName, L"Easnap - Recording", kPanelStyle,
            CW_USEDEFAULT, CW_USEDEFAULT, 1, 1, nullptr, nullptr,
            ::GetModuleHandleW(nullptr), this);
        if (panel_ == nullptr || !CreatePanelControls()) {
            Destroy();
            return false;
        }

        bool excluded = ApplyCaptureExclusion(panel_);
        for (HWND& boundary : boundaries_) {
            boundary = ::CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                kBoundaryClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                ::GetModuleHandleW(nullptr), this);
            if (boundary == nullptr) {
                Destroy();
                return false;
            }
            excluded = ApplyCaptureExclusion(boundary) && excluded;
        }
        if (!excluded) {
            platform::LogMessage(
                L"One or more recording UI windows could not be excluded from desktop capture");
        }

        const auto audio = owner_.configuration_.mp4_audio;
        ::SendMessageW(audio_system_, BM_SETCHECK,
            audio == media::AudioSource::SystemAudio || audio == media::AudioSource::Both ? BST_CHECKED : BST_UNCHECKED, 0);
        ::SendMessageW(audio_microphone_, BM_SETCHECK,
            audio == media::AudioSource::Microphone || audio == media::AudioSource::Both ? BST_CHECKED : BST_UNCHECKED, 0);
        ::SendMessageW(owner_.configuration_.format == media::RecordingFormat::Gif
                           ? format_gif_
                           : format_mp4_,
                       BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(
            owner_.configuration_.gif.preset ==
                    app::GifExportPreset::LowSize
                ? gif_low_size_
                : gif_high_quality_,
            BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(include_cursor_, BM_SETCHECK,
                       owner_.configuration_.include_cursor ? BST_CHECKED
                                                            : BST_UNCHECKED,
                       0);
        return true;
    }

    [[nodiscard]] app::RecordingConfiguration CurrentConfiguration() const {
        auto configuration = owner_.configuration_;
        if (!panel_) { return configuration; }
        configuration.mp4_audio = SelectedAudioSource();
        configuration.include_cursor = ::SendMessageW(include_cursor_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        return configuration;
    }

    void Destroy() noexcept {
        if (tooltip_ != nullptr) { ::DestroyWindow(tooltip_); tooltip_ = nullptr; }
        if (panel_ != nullptr) {
            ::KillTimer(panel_, kStatusTimerId);
            ::DestroyWindow(panel_);
            panel_ = nullptr;
        }
        for (HWND& boundary : boundaries_) {
            if (boundary != nullptr) {
                ::DestroyWindow(boundary);
                boundary = nullptr;
            }
        }
        if (ui_font_ != nullptr) {
            ::DeleteObject(ui_font_);
            ui_font_ = nullptr;
        }
        if (title_font_ != nullptr) {
            ::DeleteObject(title_font_);
            title_font_ = nullptr;
        }
        if (elapsed_font_ != nullptr) {
            ::DeleteObject(elapsed_font_);
            elapsed_font_ = nullptr;
        }
    }

    void UpdateForState(bool initial_placement = false) {
        if (panel_ == nullptr) {
            return;
        }
        const app::RecordingControlPolicy policy =
            app::RecordingControlsForState(owner_.state_);
        const bool configuring = owner_.state_ == app::SessionState::RecordingConfiguring;
        const bool finalizing = owner_.state_ == app::SessionState::RecordingFinalizing;
        const app::RecordingCapabilities capabilities =
            app::CapabilitiesForRecordingFormat(owner_.configuration_.format);

        const wchar_t* title = L"Configure recording";
        if (owner_.state_ == app::SessionState::RecordingActive) {
            title = L"Recording";
        } else if (owner_.state_ == app::SessionState::RecordingPaused) {
            title = L"Recording paused";
        } else if (finalizing) {
            title = L"Finalizing recording";
        }
        ::SetWindowTextW(title_, title);
        ::SetWindowTextW(pause_resume_, policy.can_resume ? L"Resume" : L"Pause");
        if (finalizing) {
            ::SetWindowTextW(status_,
                             L"Finishing... Your recording is being saved.");
        }

        SetControlVisible(title_, configuring);
        SetControlVisible(appearance_, configuring);
        SetControlVisible(format_label_, false);
        SetControlVisible(format_mp4_, configuring);
        SetControlVisible(format_gif_, configuring);
        SetControlVisible(gif_quality_label_,
                          configuring && capabilities.shows_gif_quality);
        SetControlVisible(gif_high_quality_,
                          configuring && capabilities.shows_gif_quality);
        SetControlVisible(gif_low_size_,
                          configuring && capabilities.shows_gif_quality);
        SetControlVisible(audio_label_, configuring && capabilities.supports_audio);
        SetControlVisible(audio_system_, configuring && capabilities.supports_audio);
        SetControlVisible(audio_microphone_, configuring && capabilities.supports_audio);
        SetControlVisible(include_cursor_, configuring && capabilities.supports_cursor);
        SetControlVisible(start_, policy.can_start);
        SetControlVisible(cancel_, policy.can_cancel);
        SetControlVisible(elapsed_, !configuring && !finalizing);
        SetControlVisible(status_, finalizing || (configuring && !error_.empty()));
        SetControlVisible(pause_resume_, policy.can_pause || policy.can_resume);
        SetControlVisible(stop_, policy.can_stop);
        ::EnableWindow(start_, policy.can_start);
        ::EnableWindow(cancel_, policy.can_cancel);
        ::EnableWindow(pause_resume_, policy.can_pause || policy.can_resume);
        ::EnableWindow(stop_, policy.can_stop);
        UpdateButtonStyle(pause_resume_, {policy.can_resume ? Icon::Play : Icon::Pause, ButtonRole::Standard, true});
        ::SendMessageW(pause_resume_,BM_SETCHECK,policy.can_resume ? BST_CHECKED:BST_UNCHECKED,0);
        ::InvalidateRect(elapsed_,nullptr,FALSE);

        ::KillTimer(panel_, kStatusTimerId);
        if (owner_.state_ == app::SessionState::RecordingActive ||
            owner_.state_ == app::SessionState::RecordingPaused || finalizing) {
            ::SetTimer(panel_, kStatusTimerId, kStatusTimerIntervalMs, nullptr);
        }

        Layout(initial_placement, false, initial_placement);
        for (HWND boundary : boundaries_) {
            ::InvalidateRect(boundary, nullptr, FALSE);
        }
        ::InvalidateRect(panel_, nullptr, FALSE);
    }

    void SetError(const std::wstring& message) {
        error_ = message;
        ::SetWindowTextW(status_, error_.c_str());
        UpdateForState();
    }

    void SetElapsed(uint64_t active_duration_100ns) {
        if (elapsed_ != nullptr &&
            (owner_.state_ == app::SessionState::RecordingActive ||
             owner_.state_ == app::SessionState::RecordingPaused)) {
            const std::wstring text = app::FormatRecordingElapsed(active_duration_100ns);
            if (text != elapsed_text_) {
                elapsed_text_ = text;
                ::SetWindowTextW(elapsed_, text.c_str());
            }
        }
    }

    void Layout(bool initial_placement, bool clamp_to_work_area,
                bool update_boundaries) {
        if (panel_ == nullptr) {
            return;
        }
        RECT current_rect{};
        const bool have_current_rect =
            !initial_placement && ::GetWindowRect(panel_, &current_rect) != FALSE;
        const platform::MonitorInfo monitor = platform::MonitorForRect(
            have_current_rect ? current_rect : owner_.region_);
        const float scale = monitor.scale();
        RECT available = monitor.work_area;
        if (owner_.workspace_hosted_) {
            available.top = (std::max)(available.top,
                monitor.bounds.top + theme::Scaled(theme::kPaletteButtonHeight + 2 * theme::kPalettePadding, scale));
        }
        const bool configuring = owner_.state_ == app::SessionState::RecordingConfiguring;
        const bool active = owner_.state_ == app::SessionState::RecordingActive ||
                            owner_.state_ == app::SessionState::RecordingPaused;
        const int desired_width = ScaleDip(active ? kActivePanelWidthDip : kPanelWidthDip, scale);
        const int client_width = std::min(desired_width, static_cast<int>(available.right - available.left));
        const bool narrow = client_width < ScaleDip(340, scale);
        const int client_height_dip = configuring ? (narrow ? 344 : 296) + (error_.empty() ? 0 : 60) : 60;
        const int client_height = ScaleDip(client_height_dip, scale);
        RECT outer{0, 0, client_width, client_height};
        AdjustPanelRect(outer, monitor.dpi);
        const int outer_width = static_cast<int>(outer.right - outer.left);
        const int outer_height = static_cast<int>(outer.bottom - outer.top);
        const LONG margin = ScaleDip(12, scale);

        LONG x = have_current_rect ? current_rect.left : owner_.region_.left;
        LONG y = have_current_rect ? current_rect.top : owner_.region_.top;
        if (initial_placement) {
            x = owner_.region_.left +
                (platform::RectWidth(owner_.region_) - outer_width) / 2;
            x = ClampCoordinate(x, monitor.work_area.left,
                                monitor.work_area.right - outer_width);
            y = owner_.region_.bottom + margin;
            if (y + outer_height > monitor.work_area.bottom) {
                y = owner_.region_.top - margin - outer_height;
            }
            if (y < monitor.work_area.top) {
                const LONG right_x = owner_.region_.right + margin;
                const LONG left_x = owner_.region_.left - margin - outer_width;
                if (right_x + outer_width <= monitor.work_area.right) {
                    x = right_x;
                } else if (left_x >= monitor.work_area.left) {
                    x = left_x;
                }
                y = ClampCoordinate(owner_.region_.bottom - outer_height,
                                    monitor.work_area.top,
                                    monitor.work_area.bottom - outer_height);
            }
            // Encoder-compatible MP4 dimensions can trim one right/bottom pixel.
            if(owner_.region_.left==monitor.bounds.left && owner_.region_.top==monitor.bounds.top &&
                monitor.bounds.right-owner_.region_.right>=0 && monitor.bounds.right-owner_.region_.right<=1 &&
                monitor.bounds.bottom-owner_.region_.bottom>=0 && monitor.bounds.bottom-owner_.region_.bottom<=1) {
                x=monitor.work_area.left+(platform::RectWidth(monitor.work_area)-outer_width)/2;
                y=monitor.work_area.top+(platform::RectHeight(monitor.work_area)-outer_height)/2;
            }
        } else if (clamp_to_work_area) {
            x = ClampCoordinate(x, monitor.work_area.left,
                                monitor.work_area.right - outer_width);
            y = ClampCoordinate(y, monitor.work_area.top,
                                monitor.work_area.bottom - outer_height);
        }

        const UINT position_flags =
            SWP_NOACTIVATE | (initial_placement ? 0 : SWP_NOZORDER) |
            (owner_.visible_ ? SWP_SHOWWINDOW : 0);
        if (owner_.workspace_hosted_) {
            x = ClampCoordinate(x, available.left, available.right - outer_width);
            y = ClampCoordinate(y, available.top, available.bottom - outer_height);
        }
        ::SetWindowPos(panel_, initial_placement ? HWND_TOPMOST : nullptr, x, y,
                       outer_width, outer_height, position_flags);
        RoundWindow(panel_);
        LayoutPanelControls(scale, client_width, client_height);
        if (update_boundaries) {
            LayoutBoundaries(scale);
        }
        if (initial_placement || clamp_to_work_area) {
            RebuildFonts(scale);
        }
    }

    void Activate() const {
        if (panel_ == nullptr) {
            return;
        }
        ::ShowWindow(panel_, SW_SHOWNORMAL);
        ::SetWindowPos(panel_, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ::SetForegroundWindow(panel_);
        if (owner_.state_ == app::SessionState::RecordingConfiguring) {
            ::SetFocus(start_);
        } else if (owner_.state_ == app::SessionState::RecordingActive ||
                   owner_.state_ == app::SessionState::RecordingPaused) {
            ::SetFocus(pause_resume_);
        }
    }

    void Hide() const noexcept {
        if (panel_ != nullptr) {
            ::KillTimer(panel_, kStatusTimerId);
            ::ShowWindow(panel_, SW_HIDE);
        }
        for (HWND boundary : boundaries_) {
            if (boundary != nullptr) {
                ::ShowWindow(boundary, SW_HIDE);
            }
        }
    }

    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const {
        return panel_ != nullptr && owner_.visible_ &&
               (message.hwnd == panel_ || ::IsChild(panel_, message.hwnd)) &&
               (ConsumePopupInput(message.message, message.wParam) ||
                ActivateFocusedButton(message, panel_) || ::IsDialogMessageW(panel_, &message) != FALSE);
    }

    [[nodiscard]] HWND hwnd() const noexcept { return panel_; }

private:
    [[nodiscard]] static bool EnsureWindowClasses() {
        static const bool registered = [] {
            WNDCLASSEXW panel_class{};
            panel_class.cbSize = sizeof(panel_class);
            panel_class.style = CS_HREDRAW | CS_VREDRAW;
            panel_class.lpfnWndProc = PanelWindowProc;
            panel_class.hInstance = ::GetModuleHandleW(nullptr);
            panel_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            panel_class.hbrBackground = nullptr;
            panel_class.lpszClassName = kPanelClassName;
            if (::RegisterClassExW(&panel_class) == 0 &&
                ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                return false;
            }

            WNDCLASSEXW boundary_class{};
            boundary_class.cbSize = sizeof(boundary_class);
            boundary_class.style = CS_HREDRAW | CS_VREDRAW;
            boundary_class.lpfnWndProc = BoundaryWindowProc;
            boundary_class.hInstance = ::GetModuleHandleW(nullptr);
            // These windows are visual-only. Leaving their class cursor unset
            // prevents a transparent boundary from briefly replacing the
            // underlying application's cursor during composition updates.
            boundary_class.hCursor = nullptr;
            boundary_class.lpszClassName = kBoundaryClassName;
            return ::RegisterClassExW(&boundary_class) != 0 ||
                   ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }();
        return registered;
    }

    [[nodiscard]] static Window* FromHandle(HWND hwnd, UINT message,
                                             LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            auto* self = static_cast<Window*>(create->lpCreateParams);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(self));
            return self;
        }
        return reinterpret_cast<Window*>(
            ::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT CALLBACK PanelWindowProc(HWND hwnd, UINT message,
                                             WPARAM wparam, LPARAM lparam) {
        if (Window* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandlePanelMessage(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK BoundaryWindowProc(HWND hwnd, UINT message,
                                                WPARAM wparam, LPARAM lparam) {
        if (Window* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandleBoundaryMessage(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandlePanelMessage(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam) {
        if (ConsumePopupInput(message, wparam)) { return 0; }
        switch (message) {
            case WM_NCDESTROY:
                // Destroying an owner also destroys its owned HWNDs. The C++
                // view can survive until deferred cleanup; never retain handles
                // to the panel or its already-destroyed controls in that gap.
                panel_ = nullptr;
                owner_.visible_ = false;
                for (HWND* control : {&title_, &elapsed_, &status_, &format_label_,
                     &format_mp4_, &format_gif_, &gif_quality_label_, &gif_high_quality_,
                     &gif_low_size_, &audio_label_, &appearance_, &tooltip_, &audio_system_,
                     &audio_microphone_, &include_cursor_, &start_, &cancel_, &pause_resume_, &stop_}) {
                    *control = nullptr;
                }
                for (HWND boundary : boundaries_) if (boundary) { ::ShowWindow(boundary, SW_HIDE); }
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            case theme::kChangedMessage:
                ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                for (HWND boundary : boundaries_) { ::InvalidateRect(boundary, nullptr, FALSE); }
                return 0;
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            case WM_PRINTCLIENT: {
                PAINTSTRUCT ps{}; HDC dc=message==WM_PAINT ? ::BeginPaint(hwnd,&ps):reinterpret_cast<HDC>(wparam);
                RECT client{}; ::GetClientRect(hwnd,&client); const float scale=::GetDpiForWindow(hwnd)/96.0f;
                PaintBuffer buffer(dc,client); dc=buffer.dc();
                theme::DrawPanel(dc,client,scale);
                if(owner_.state_!=app::SessionState::RecordingConfiguring) {
                    theme::DrawGrip(dc,{0,0,theme::Scaled(28,scale),client.bottom},scale);
                }
                buffer.Present();
                if(message==WM_PAINT) { ::EndPaint(hwnd,&ps); } return 0;
            }
            case WM_NCHITTEST: {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ::ScreenToClient(hwnd, &point);
                const HWND child=::ChildWindowFromPointEx(hwnd,point,CWP_SKIPINVISIBLE|CWP_SKIPDISABLED|CWP_SKIPTRANSPARENT);
                // Informational labels and every unused part of an island can
                // drag it. Only actual input controls reserve their hit area.
                wchar_t class_name[32]{};
                if(child && child!=hwnd) { ::GetClassNameW(child,class_name,ARRAYSIZE(class_name)); }
                return child && child!=hwnd && ::lstrcmpiW(class_name,L"STATIC")!=0 ? HTCLIENT:HTCAPTION;
            }
            case WM_NCLBUTTONDBLCLK: return 0;
            case WM_COMMAND:
                HandleCommand(LOWORD(wparam));
                return 0;
            case WM_CLOSE:
                HandleCancelOrStop();
                return 0;
            case WM_TIMER:
                if (wparam == kStatusTimerId && owner_.callbacks_.on_status_tick) {
                    owner_.callbacks_.on_status_tick();
                }
                return 0;
            case WM_MOVING: {
                if (owner_.workspace_hosted_) {
                    auto* proposed = reinterpret_cast<RECT*>(lparam);
                    const auto monitor = platform::MonitorForRect(*proposed);
                    RECT available = monitor.work_area;
                    available.top = std::max(available.top, monitor.bounds.top +
                        theme::Scaled(theme::kPaletteButtonHeight + 2 * theme::kPalettePadding, monitor.scale()));
                    *proposed = platform::ClampRectToBounds(*proposed, available);
                    return TRUE;
                }
                break;
            }
            case WM_DPICHANGED:
                owner_.RefreshLayout();
                return 0;
            case WM_CTLCOLORSTATIC: {
                const COLORREF text = reinterpret_cast<HWND>(lparam) == elapsed_
                    ? (owner_.state_ == app::SessionState::RecordingPaused ? theme::Current().paused : theme::Current().text)
                    : (reinterpret_cast<HWND>(lparam) == status_ && !error_.empty() ? theme::Current().error : theme::Current().muted_text);
                return ControlBackground(wparam, text);
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandleBoundaryMessage(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam) const {
        switch (message) {
            case WM_NCHITTEST:
                return HTTRANSPARENT;
            case WM_SETCURSOR:
                return FALSE;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = ::BeginPaint(hwnd, &paint);
                RECT client{};
                ::GetClientRect(hwnd, &client);
                theme::FillSolid(dc, client, BoundaryColor());
                ::EndPaint(hwnd, &paint);
                return 0;
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    [[nodiscard]] bool CreatePanelControls() {
        title_ = CreateControl(L"STATIC", L"Record screen", SS_LEFT, kIdTitle);
        start_ = CreateControl(L"BUTTON", L"Start recording", WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON, IDOK);
        appearance_ = CreateControl(L"BUTTON", L"Appearance", WS_TABSTOP | BS_PUSHBUTTON, kIdAppearance);
        format_label_ = CreateControl(L"STATIC", L"Format", SS_LEFT, kIdFormatLabel);
        format_mp4_ = CreateControl(L"BUTTON", L"MP4", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdFormatMp4);
        format_gif_ = CreateControl(L"BUTTON", L"GIF", WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdFormatGif);
        audio_label_ = CreateControl(L"STATIC", L"Audio", SS_LEFT, kIdAudioLabel);
        audio_system_ = CreateControl(L"BUTTON", L"System audio", WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE, kIdAudioSystem);
        audio_microphone_ = CreateControl(L"BUTTON", L"Microphone", WS_TABSTOP | BS_AUTOCHECKBOX | BS_PUSHLIKE, kIdAudioMicrophone);
        gif_quality_label_ = CreateControl(L"STATIC", L"GIF quality", SS_LEFT, kIdGifQualityLabel);
        gif_high_quality_ = CreateControl(L"BUTTON", L"High quality", WS_TABSTOP | WS_GROUP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdGifHighQuality);
        gif_low_size_ = CreateControl(L"BUTTON", L"Low size", WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE, kIdGifLowSize);
        include_cursor_ = CreateControl(L"BUTTON", L"Include cursor", WS_TABSTOP | WS_GROUP | BS_AUTOCHECKBOX | BS_PUSHLIKE, kIdIncludeCursor);
        cancel_ = CreateControl(L"BUTTON", L"Cancel", WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        elapsed_ = CreateControl(L"STATIC", L"00:00:00", SS_CENTER | SS_CENTERIMAGE, kIdElapsed);
        status_ = CreateControl(L"STATIC", L"", SS_LEFT, kIdStatus);
        pause_resume_ = CreateControl(L"BUTTON", L"Pause", WS_TABSTOP | BS_CHECKBOX | BS_PUSHLIKE, kIdPauseResume);
        stop_ = CreateControl(L"BUTTON", L"Stop", WS_TABSTOP | BS_PUSHBUTTON, kIdStop);
        for (HWND control : {title_, start_, appearance_, format_label_, format_mp4_, format_gif_, audio_label_,
             audio_system_, audio_microphone_, gif_quality_label_, gif_high_quality_, gif_low_size_, include_cursor_,
             cancel_, elapsed_, status_, pause_resume_, stop_}) {
            if (!control) { return false; }
        }
        UpdateButtonStyle(start_, {Icon::Record, ButtonRole::Primary});
        UpdateButtonStyle(appearance_, {Icon::Appearance, ButtonRole::Standard, true});
        UpdateButtonStyle(audio_system_, {Icon::SystemAudio});
        UpdateButtonStyle(audio_microphone_, {Icon::Microphone});
        UpdateButtonStyle(include_cursor_, {Icon::Cursor});
        UpdateButtonStyle(pause_resume_, {Icon::Pause, ButtonRole::Standard, true});
        UpdateButtonStyle(stop_, {Icon::Stop, ButtonRole::Destructive});
        tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0,0,0,0, panel_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (tooltip_) {
            TOOLINFOW info{sizeof(info)};
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = panel_;
            info.uId = reinterpret_cast<UINT_PTR>(appearance_);
            info.lpszText = const_cast<wchar_t*>(L"Appearance - Light or Dark");
            ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
            StyleTooltip(tooltip_);
        }
        return true;
    }

    [[nodiscard]] HWND CreateControl(const wchar_t* class_name,
                                     const wchar_t* text, DWORD style,
                                     int id) const {
        HWND control = ::CreateWindowExW(
            0, class_name, text, WS_CHILD | style, 0, 0, 1, 1, panel_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            ::GetModuleHandleW(nullptr), nullptr);
        if (control && ::lstrcmpW(class_name, L"BUTTON") == 0 && !StyleButton(control)) {
            ::DestroyWindow(control);
            return nullptr;
        }
        if(control && ::lstrcmpW(class_name,L"STATIC")==0 && !StyleLabel(control)) { ::DestroyWindow(control); return nullptr; }
        return control;
    }

    void HandleCommand(int id) {
        const app::RecordingControlPolicy policy =
            app::RecordingControlsForState(owner_.state_);
        switch (id) {
            case kIdAppearance:
                if (policy.can_start) { ShowAppearanceMenu(appearance_); }
                break;
            case kAppearanceLight:
            case kAppearanceDark:
                if (owner_.callbacks_.on_appearance) {
                    owner_.callbacks_.on_appearance(id == kAppearanceDark
                        ? app::AppearanceTheme::Dark : app::AppearanceTheme::Light);
                }
                break;
            case kIdAudioSystem:
            case kIdAudioMicrophone:
                if (policy.can_start) { owner_.configuration_.mp4_audio = SelectedAudioSource(); }
                break;
            case kIdIncludeCursor:
                if (policy.can_start) { owner_.configuration_.include_cursor = ::SendMessageW(include_cursor_, BM_GETCHECK, 0, 0) == BST_CHECKED; }
                break;
            case IDOK:
                if (policy.can_start && owner_.callbacks_.on_start) {
                    owner_.configuration_.mp4_audio = SelectedAudioSource();
                    owner_.configuration_.include_cursor =
                        ::SendMessageW(include_cursor_, BM_GETCHECK, 0, 0) ==
                        BST_CHECKED;
                    owner_.callbacks_.on_start(owner_.configuration_);
                }
                break;
            case kIdFormatMp4:
            case kIdFormatGif:
                if (!policy.can_start) { break; }
                owner_.configuration_.mp4_audio = SelectedAudioSource();
                owner_.configuration_.format =
                    id == kIdFormatGif ? media::RecordingFormat::Gif
                                       : media::RecordingFormat::Mp4;
                UpdateForState();
                break;
            case kIdGifHighQuality:
                if (!policy.can_start) { break; }
                owner_.configuration_.gif.preset =
                    app::GifExportPreset::HighQuality;
                break;
            case kIdGifLowSize:
                if (!policy.can_start) { break; }
                owner_.configuration_.gif.preset =
                    app::GifExportPreset::LowSize;
                break;
            case IDCANCEL:
                HandleCancelOrStop();
                break;
            case kIdPauseResume:
                if (policy.can_pause && owner_.callbacks_.on_pause) {
                    owner_.callbacks_.on_pause();
                } else if (policy.can_resume && owner_.callbacks_.on_resume) {
                    owner_.callbacks_.on_resume();
                }
                break;
            case kIdStop:
                if (policy.can_stop && owner_.callbacks_.on_stop) {
                    owner_.callbacks_.on_stop();
                }
                break;
            default:
                break;
        }
    }

    void HandleCancelOrStop() {
        const app::RecordingControlPolicy policy =
            app::RecordingControlsForState(owner_.state_);
        if (policy.can_cancel && owner_.callbacks_.on_cancel) {
            owner_.callbacks_.on_cancel();
        } else if (policy.can_stop && owner_.callbacks_.on_stop) {
            owner_.callbacks_.on_stop();
        }
    }

    [[nodiscard]] media::AudioSource SelectedAudioSource() const noexcept {
        const bool system = ::SendMessageW(audio_system_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool microphone = ::SendMessageW(audio_microphone_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        return system ? (microphone ? media::AudioSource::Both : media::AudioSource::SystemAudio)
                      : (microphone ? media::AudioSource::Microphone : media::AudioSource::None);
    }

    void LayoutPanelControls(float scale, int client_width, int client_height) {
        const int width = static_cast<int>(static_cast<float>(client_width) / scale);
        const bool narrow = width < 340;
        const auto move = [scale](HWND control, int x, int y, int w, int h) {
            ::MoveWindow(control, static_cast<int>(x * scale), static_cast<int>(y * scale),
                ScaleDip(w, scale), ScaleDip(h, scale), TRUE);
        };
        if (owner_.state_ == app::SessionState::RecordingConfiguring) {
            const int content = width - 32;
            move(title_, 20, 16, content - 48, 24);
            move(appearance_, width - 52, 8, 36, 36);
            move(start_, 16, 48, content, 48);
            move(format_mp4_, 16, 104, content / 2 - 2, 36);
            move(format_gif_, 18 + content / 2, 104, content / 2 - 2, 36);
            move(audio_label_, 20, 152, content, 20);
            move(gif_quality_label_, 20, 152, content, 20);
            const int half = narrow ? content : content / 2 - 2;
            move(audio_system_, 16, 176, half, 40);
            move(gif_high_quality_, 16, 176, half, 40);
            move(audio_microphone_, narrow ? 16 : 18 + content / 2, narrow ? 224 : 176, half, 40);
            move(gif_low_size_, narrow ? 16 : 18 + content / 2, narrow ? 224 : 176, half, 40);
            move(include_cursor_, 16, narrow ? 288 : 240, content - 88, 40);
            move(cancel_, width - 96, narrow ? 288 : 240, 80, 40);
            if (!error_.empty()) { move(status_, 20, narrow ? 336 : 288, content - 8, 56); }
        } else if (owner_.state_ == app::SessionState::RecordingFinalizing) {
            move(status_, 36, 8, width - 52, 44);
        } else {
            move(pause_resume_, 28, 10, 40, 40);
            move(elapsed_, 72, 10, width - 164, 40);
            move(stop_, width - 88, 10, 80, 40);
        }
        static_cast<void>(client_height);
    }

    void LayoutBoundaries(float scale) {
        if (owner_.workspace_hosted_) {
            for (HWND boundary : boundaries_) { ::ShowWindow(boundary, SW_HIDE); }
            return;
        }
        const int thickness = ScaleDip(3, scale);
        const RECT virtual_screen = platform::VirtualScreenBounds();
        const RECT& region = owner_.region_;
        const int width = static_cast<int>(platform::RectWidth(region));
        const int height = static_cast<int>(platform::RectHeight(region));
        if (width <= 0 || height <= 0) {
            return;
        }

        const LONG top = region.top - thickness >= virtual_screen.top
                             ? region.top - thickness
                             : region.top;
        const LONG bottom = region.bottom + thickness <= virtual_screen.bottom
                                ? region.bottom
                                : region.bottom - thickness;
        const LONG left = region.left - thickness >= virtual_screen.left
                              ? region.left - thickness
                              : region.left;
        const LONG right = region.right + thickness <= virtual_screen.right
                               ? region.right
                               : region.right - thickness;
        const UINT flags = SWP_NOACTIVATE | (owner_.visible_ ? SWP_SHOWWINDOW : 0);
        ::SetWindowPos(boundaries_[0], HWND_TOPMOST, region.left, top, width,
                       thickness, flags);
        ::SetWindowPos(boundaries_[1], HWND_TOPMOST, region.left, bottom, width,
                       thickness, flags);
        ::SetWindowPos(boundaries_[2], HWND_TOPMOST, left, region.top, thickness,
                       height, flags);
        ::SetWindowPos(boundaries_[3], HWND_TOPMOST, right, region.top, thickness,
                       height, flags);
    }

    void RebuildFonts(float scale) {
        HFONT new_ui = theme::CreateUiFont(13.0f, scale, false);
        HFONT new_title = theme::CreateUiFont(15.0f, scale, true);
        HFONT new_elapsed = theme::CreateUiFont(15.0f, scale, true);
        if (new_ui == nullptr || new_title == nullptr || new_elapsed == nullptr) {
            if (new_ui != nullptr) {
                ::DeleteObject(new_ui);
            }
            if (new_title != nullptr) {
                ::DeleteObject(new_title);
            }
            if (new_elapsed != nullptr) {
                ::DeleteObject(new_elapsed);
            }
            return;
        }

        for (HWND control : {audio_label_, audio_system_, appearance_, gif_quality_label_, gif_high_quality_, gif_low_size_,
                             audio_microphone_, format_label_,
                             format_mp4_, format_gif_, include_cursor_, start_, cancel_,
                             pause_resume_, stop_, status_}) {
            SetControlFont(control, new_ui);
        }
        SetControlFont(title_, new_title);
        SetControlFont(elapsed_, new_elapsed);

        if (ui_font_ != nullptr) {
            ::DeleteObject(ui_font_);
        }
        if (title_font_ != nullptr) {
            ::DeleteObject(title_font_);
        }
        if (elapsed_font_ != nullptr) {
            ::DeleteObject(elapsed_font_);
        }
        ui_font_ = new_ui;
        title_font_ = new_title;
        elapsed_font_ = new_elapsed;
    }

    [[nodiscard]] COLORREF BoundaryColor() const noexcept {
        if (owner_.state_ == app::SessionState::RecordingPaused) {
            return theme::Current().paused;
        }
        return theme::Current().record_border;
    }

    RecordingControls& owner_;
    HWND panel_{nullptr};
    std::array<HWND, 4> boundaries_{};
    HWND title_{nullptr};
    HWND elapsed_{nullptr};
    HWND status_{nullptr};
    HWND format_label_{nullptr};
    HWND format_mp4_{nullptr};
    HWND format_gif_{nullptr};
    HWND gif_quality_label_{nullptr};
    HWND gif_high_quality_{nullptr};
    HWND gif_low_size_{nullptr};
    HWND audio_label_{nullptr};
    HWND appearance_{nullptr};
    HWND tooltip_{nullptr};
    HWND audio_system_{nullptr};
    HWND audio_microphone_{nullptr};
    HWND include_cursor_{nullptr};
    HWND start_{nullptr};
    HWND cancel_{nullptr};
    HWND pause_resume_{nullptr};
    HWND stop_{nullptr};
    HFONT ui_font_{nullptr};
    HFONT title_font_{nullptr};
    HFONT elapsed_font_{nullptr};
    std::wstring elapsed_text_{L"00:00:00"};
    std::wstring error_;
};

RecordingControls::RecordingControls(Callbacks callbacks)
    : callbacks_(std::move(callbacks)) {}

RecordingControls::~RecordingControls() { Destroy(); }

bool RecordingControls::ShowConfiguring(
    const RECT& region, app::RecordingConfiguration configuration, bool workspace_hosted, HWND workspace_owner) {
    if (!app::IsValidRecordingConfiguration(configuration)) {
        return false;
    }
    workspace_hosted_ = workspace_hosted;
    region_ = region;
    if (window_ && !window_->hwnd()) { window_.reset(); }
    if (window_) {
        ::SetWindowLongPtrW(window_->hwnd(), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(workspace_owner));
        visible_ = true;
        window_->UpdateForState();
        window_->Activate();
        return true;
    }
    configuration_ = configuration;
    state_ = app::SessionState::RecordingConfiguring;
    window_ = std::make_unique<Window>(*this);
    if (!window_->Create()) {
        window_.reset();
        return false;
    }
    ::SetWindowLongPtrW(window_->hwnd(), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(workspace_owner));
    visible_ = true;
    window_->UpdateForState(true);
    window_->Activate();
    return true;
}

void RecordingControls::SetRegion(const RECT& region) {
    if (::EqualRect(&region_, &region)) { return; }
    region_ = region;
    // In a workspace, the overlay owns the frame and the panel stays where the
    // user placed it. Its controls/fonts have no dependency on frame geometry.
    // RetireWorkspace lays out the live recording boundary using the latest region.
    if (window_ && visible_ && !workspace_hosted_) { window_->Layout(false, true, true); }
}

void RecordingControls::RetireWorkspace() {
    workspace_hosted_ = false;
    // Recording outlives the selection workspace; release ownership before its
    // overlay windows are destroyed.
    if (window_) { ::SetWindowLongPtrW(window_->hwnd(), GWLP_HWNDPARENT, 0); }
    if (window_) { window_->Layout(false, true, true); }
}

app::RecordingConfiguration RecordingControls::configuration() const {
    return window_ ? window_->CurrentConfiguration() : configuration_;
}

void RecordingControls::SetSessionState(app::SessionState state) {
    state_ = state;
    if (window_ != nullptr) {
        window_->UpdateForState();
    }
}

void RecordingControls::SetElapsed(uint64_t active_duration_100ns) {
    if (window_ != nullptr) {
        window_->SetElapsed(active_duration_100ns);
    }
}

void RecordingControls::SetError(const std::wstring& message) {
    if (window_) { window_->SetError(message); }
}

void RecordingControls::Activate() {
    if (window_ != nullptr && visible_) {
        window_->Activate();
    }
}

void RecordingControls::RefreshLayout() {
    if (window_ != nullptr) {
        window_->Layout(false, true, true);
    }
}

void RecordingControls::Hide() {
    visible_ = false;
    if (window_ != nullptr) {
        window_->Hide();
    }
}

void RecordingControls::Destroy() {
    visible_ = false;
    window_.reset();
}

bool RecordingControls::ProcessDialogMessage(MSG& message) const {
    return window_ != nullptr && window_->ProcessDialogMessage(message);
}

HWND RecordingControls::hwnd() const noexcept {
    return window_ != nullptr ? window_->hwnd() : nullptr;
}

}  // namespace easnap::ui

