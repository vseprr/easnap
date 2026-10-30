// Easnap - Scrollshot island and live preview implementation.
#include "ui/scrollshot_controls.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <utility>

#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/scrollshot_layout.h"
#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {

constexpr wchar_t kIslandClassName[] = L"EasnapScrollshotControls";
constexpr wchar_t kPreviewClassName[] = L"EasnapScrollshotPreview";

constexpr int kIdHelp = 200;
constexpr int kIdEdit = 201;
constexpr int kIdSave = 202;
constexpr int kIdSize = 210;
constexpr int kIdMessage = 211;
// Start and Copy are IDOK, Discard is IDCANCEL: Enter and Esc reach them
// through the native dialog navigation.

constexpr DWORD kIslandStyle = WS_POPUP | WS_CLIPCHILDREN;
constexpr DWORD kIslandExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;
constexpr DWORD kPreviewExStyle =
    WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED;

// Metrics in device-independent pixels.
constexpr float kPreviewWidth = 160.0f;
constexpr float kPreviewPadding = 4.0f;
constexpr float kIslandHeight = 72.0f;
constexpr float kStartHeight = 56.0f;
constexpr float kGrip = 24.0f;
constexpr float kButton = 40.0f;
constexpr float kStatusWidth = 250.0f;
constexpr float kGap = 12.0f;

[[nodiscard]] LONG TextWidth(HFONT font, const wchar_t* text) {
    LONG width = 0;
    if (HDC dc = ::CreateCompatibleDC(nullptr)) {
        const HGDIOBJ previous = ::SelectObject(dc, font);
        SIZE size{};
        if (::GetTextExtentPoint32W(dc, text, ::lstrlenW(text), &size)) { width = size.cx; }
        ::SelectObject(dc, previous);
        ::DeleteDC(dc);
    }
    return width;
}

}  // namespace

class ScrollshotControls::Window final {
public:
    explicit Window(ScrollshotControls& owner) : owner_(owner) {}
    ~Window() { Destroy(); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool Create(HWND workspace_owner) {
        if (!EnsureWindowClasses()) { return false; }
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        island_ = ::CreateWindowExW(kIslandExStyle, kIslandClassName, strings::Get(IDS_SCROLLSHOT_PANEL_TITLE),
                                    kIslandStyle, 0, 0, 1, 1, workspace_owner, nullptr, instance, this);
        preview_ = ::CreateWindowExW(kPreviewExStyle, kPreviewClassName, strings::Get(IDS_SCROLLSHOT_PREVIEW_TITLE),
                                     WS_POPUP, 0, 0, 1, 1, workspace_owner, nullptr, instance, this);
        if (island_ == nullptr || preview_ == nullptr || !CreateControls()) {
            Destroy();
            return false;
        }
        ::SetLayeredWindowAttributes(preview_, 0, 255, LWA_ALPHA);
        // Neither surface may ever appear in a captured picture.
        const bool excluded = ExcludeFromCapture(island_) && ExcludeFromCapture(preview_);
        if (!excluded) {
            platform::LogMessage(L"A Scrollshot surface could not be excluded from desktop capture");
        }
        return true;
    }

    void Destroy() noexcept {
        if (tooltip_ != nullptr) { ::DestroyWindow(tooltip_); tooltip_ = nullptr; }
        if (preview_ != nullptr) {
            ::SetWindowLongPtrW(preview_, GWLP_USERDATA, 0);
            ::DestroyWindow(preview_);
            preview_ = nullptr;
        }
        if (island_ != nullptr) {
            ::SetWindowLongPtrW(island_, GWLP_USERDATA, 0);
            ::DestroyWindow(island_);
            island_ = nullptr;
        }
        ClearControlHandles();
        for (HFONT* font : {&ui_font_, &strong_font_}) {
            if (*font != nullptr) { ::DeleteObject(*font); *font = nullptr; }
        }
    }

    void ApplyView() {
        const bool start = owner_.view_ == View::Start;
        for (HWND control : {start_, help_}) { ::ShowWindow(control, start ? SW_SHOW : SW_HIDE); }
        for (HWND control : {size_, message_, edit_, save_, discard_, copy_}) {
            ::ShowWindow(control, start ? SW_HIDE : SW_SHOW);
        }
        if (start) { ::ShowWindow(preview_, SW_HIDE); }
        user_positioned_ = false;
        Layout();
    }

    /// Places both surfaces for the current frame, monitors and DPI.
    void Layout() {
        if (island_ == nullptr) { return; }
        std::vector<platform::MonitorInfo> monitors = platform::EnumerateMonitors();
        if (monitors.empty()) { return; }
        platform::MonitorInfo anchor = platform::MonitorForRect(owner_.frame_);
        const float scale = anchor.scale();
        if (scale != scale_ || ui_font_ == nullptr) { RebuildFonts(scale); }
        // The mode options sit at the top of a monitor; keep clear of them.
        for (platform::MonitorInfo& monitor : monitors) {
            monitor.work_area.top = std::max(monitor.work_area.top, monitor.bounds.top +
                theme::Scaled(theme::kPaletteButtonHeight + 2 * theme::kPalettePadding, monitor.scale()));
        }

        const bool start = owner_.view_ == View::Start;
        const LONG padding = theme::Scaled(8, scale);
        const LONG button = theme::Scaled(kButton, scale);
        const LONG grip = theme::Scaled(kGrip, scale);
        const LONG height = theme::Scaled(start ? kStartHeight : kIslandHeight, scale);
        const LONG start_width = TextWidth(ui_font_, strings::Get(IDS_SCROLLSHOT_START)) + theme::Scaled(64, scale);
        const LONG status_width = theme::Scaled(kStatusWidth, scale);
        const LONG button_gap = theme::Scaled(4, scale);
        const LONG width = start ? grip + start_width + button_gap + button + padding
                                 : grip + status_width + padding + 4 * button + 3 * button_gap + padding;
        const LONG preview_width = PreviewPixelWidth(owner_.frame_) + 2 * theme::Scaled(kPreviewPadding, scale);
        const LONG preview_room = std::max<LONG>(preview_width, platform::RectHeight(anchor.work_area));
        const ScrollshotPlacement placement = PlaceScrollshotSurfaces(
            owner_.frame_, monitors, SIZE{width, height}, preview_width, preview_room, theme::Scaled(kGap, scale));
        owner_.island_over_frame_ = placement.island_over_frame;
        owner_.preview_over_frame_ = placement.preview_over_frame;
        preview_area_ = placement.preview;

        RECT island = placement.island;
        if (user_positioned_) {
            // Keep where the user dragged it, as long as that is still on a monitor.
            RECT current{};
            ::GetWindowRect(island_, &current);
            const platform::MonitorInfo monitor = platform::MonitorForRect(current);
            island = platform::ClampRectToBounds(
                RECT{current.left, current.top, current.left + width, current.top + height}, monitor.work_area);
        }
        ::SetWindowPos(island_, HWND_TOPMOST, island.left, island.top, width, height,
                       SWP_NOACTIVATE | (owner_.visible_ ? SWP_SHOWWINDOW : 0));
        RoundWindow(island_);

        const LONG button_top = (height - button) / 2;
        if (start) {
            ::MoveWindow(start_, grip, button_top, start_width, button, TRUE);
            ::MoveWindow(help_, grip + start_width + button_gap, button_top, button, button, TRUE);
        } else {
            const LONG text_left = grip + theme::Scaled(4, scale);
            const LONG text_width = status_width - theme::Scaled(4, scale);
            ::MoveWindow(size_, text_left, theme::Scaled(8, scale), text_width, theme::Scaled(20, scale), TRUE);
            ::MoveWindow(message_, text_left, theme::Scaled(28, scale), text_width, theme::Scaled(36, scale), TRUE);
            LONG x = grip + status_width + padding;
            for (HWND control : {edit_, save_, discard_, copy_}) {
                ::MoveWindow(control, x, button_top, button, button, TRUE);
                x += button + button_gap;
            }
        }
        LayoutPreview();
        ::InvalidateRect(island_, nullptr, FALSE);
    }

    void SetStatus(const std::wstring& size, const std::wstring& message, bool warning) {
        if (island_ == nullptr) { return; }
        warning_ = warning;
        ::SetWindowTextW(size_, size.c_str());
        ::SetWindowTextW(message_, message.c_str());
        ::InvalidateRect(message_, nullptr, FALSE);
    }

    void SetPreview(const scrollshot::ScrollshotPreview& preview) {
        if (preview_ == nullptr || preview.width <= 0 || preview.height <= 0) { return; }
        image_ = preview;
        LayoutPreview();
        ::InvalidateRect(preview_, nullptr, FALSE);
    }

    void SetBusy(bool busy) {
        for (HWND control : {edit_, save_, discard_, copy_}) {
            if (control != nullptr) { ::EnableWindow(control, busy ? FALSE : TRUE); }
        }
    }

    void Activate() const {
        if (island_ == nullptr) { return; }
        ::SetWindowPos(island_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        ::SetForegroundWindow(island_);
        ::SetFocus(owner_.view_ == View::Start ? start_ : copy_);
    }

    void Show() const {
        if (island_ != nullptr) { ::ShowWindow(island_, SW_SHOWNOACTIVATE); }
    }

    void Hide() const noexcept {
        if (island_ != nullptr) { ::ShowWindow(island_, SW_HIDE); }
        if (preview_ != nullptr) { ::ShowWindow(preview_, SW_HIDE); }
    }

    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const {
        if (island_ == nullptr || !owner_.visible_ ||
            !(message.hwnd == island_ || ::IsChild(island_, message.hwnd))) {
            return false;
        }
        if (ConsumePopupInput(message.message, message.wParam)) { return true; }
        if (message.message == WM_KEYDOWN && (::GetKeyState(VK_CONTROL) & 0x8000) != 0 &&
            owner_.view_ == View::Capture) {
            if (message.wParam == 'S') { owner_.Dispatch(Action::Save); return true; }
            if (message.wParam == 'C') { owner_.Dispatch(Action::Copy); return true; }
        }
        return ActivateFocusedButton(message, island_) || ::IsDialogMessageW(island_, &message) != FALSE;
    }

    [[nodiscard]] HWND island() const noexcept { return island_; }
    [[nodiscard]] HWND preview() const noexcept { return preview_; }

private:
    [[nodiscard]] static bool EnsureWindowClasses() {
        static const bool registered = [] {
            WNDCLASSEXW island{};
            island.cbSize = sizeof(island);
            island.style = CS_HREDRAW | CS_VREDRAW;
            island.lpfnWndProc = IslandProc;
            island.hInstance = ::GetModuleHandleW(nullptr);
            island.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
            island.lpszClassName = kIslandClassName;
            WNDCLASSEXW preview = island;
            preview.lpfnWndProc = PreviewProc;
            // Visual only: no class cursor, so it never replaces the cursor of
            // the application underneath.
            preview.hCursor = nullptr;
            preview.lpszClassName = kPreviewClassName;
            return ::RegisterClassExW(&island) != 0 && ::RegisterClassExW(&preview) != 0;
        }();
        return registered;
    }

    [[nodiscard]] static Window* FromHandle(HWND hwnd, UINT message, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        return reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT CALLBACK IslandProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (Window* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandleIsland(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (Window* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandlePreview(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void ClearControlHandles() noexcept {
        for (HWND* control : {&start_, &help_, &size_, &message_, &edit_, &save_, &discard_, &copy_}) {
            *control = nullptr;
        }
    }

    LRESULT HandleIsland(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (ConsumePopupInput(message, wparam)) { return 0; }
        switch (message) {
            case WM_NCDESTROY:
                // Destroying the workspace owner destroys this window too. The
                // C++ object can outlive it until deferred cleanup.
                island_ = nullptr;
                tooltip_ = nullptr;
                owner_.visible_ = false;
                ClearControlHandles();
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            case theme::kChangedMessage:
                ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
                if (preview_ != nullptr) { ::InvalidateRect(preview_, nullptr, FALSE); }
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT:
            case WM_PRINTCLIENT: {
                PAINTSTRUCT paint{};
                HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &paint) : reinterpret_cast<HDC>(wparam);
                RECT client{};
                ::GetClientRect(hwnd, &client);
                const float scale = static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f;
                {
                    PaintBuffer buffer(dc, client);
                    theme::DrawPanel(buffer.dc(), client, scale);
                    theme::DrawGrip(buffer.dc(), {0, 0, theme::Scaled(kGrip, scale), client.bottom}, scale);
                }
                if (message == WM_PAINT) { ::EndPaint(hwnd, &paint); }
                return 0;
            }
            case WM_NCHITTEST: {
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ::ScreenToClient(hwnd, &point);
                const HWND child = ::ChildWindowFromPointEx(
                    hwnd, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
                // The grip, the status text and every unused part drag the island.
                wchar_t class_name[32]{};
                if (child != nullptr && child != hwnd) { ::GetClassNameW(child, class_name, ARRAYSIZE(class_name)); }
                return child != nullptr && child != hwnd && ::lstrcmpiW(class_name, L"STATIC") != 0 ? HTCLIENT : HTCAPTION;
            }
            case WM_NCLBUTTONDBLCLK:
                return 0;
            case WM_ENTERSIZEMOVE:
                user_positioned_ = true;
                return 0;
            case WM_MOVING:
                if (auto* proposed = reinterpret_cast<RECT*>(lparam)) {
                    *proposed = platform::ClampRectToBounds(*proposed, platform::MonitorForRect(*proposed).work_area);
                    return TRUE;
                }
                break;
            case WM_DPICHANGED:
                if (const auto* suggested = reinterpret_cast<const RECT*>(lparam)) {
                    ::SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                                   static_cast<int>(platform::RectWidth(*suggested)),
                                   static_cast<int>(platform::RectHeight(*suggested)), SWP_NOZORDER | SWP_NOACTIVATE);
                }
                Layout();
                return 0;
            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case IDOK: owner_.Dispatch(owner_.view_ == View::Start ? Action::Start : Action::Copy); break;
                    case IDCANCEL: owner_.Dispatch(Action::Discard); break;
                    case kIdHelp: owner_.Dispatch(Action::Help); break;
                    case kIdEdit: owner_.Dispatch(Action::Edit); break;
                    case kIdSave: owner_.Dispatch(Action::Save); break;
                    default: break;
                }
                return 0;
            case WM_CLOSE:
                owner_.Dispatch(Action::Discard);
                return 0;
            case WM_MOUSEWHEEL: {
                // The wheel arrives here only when Windows routes it to the
                // focused window instead of the one under the pointer.
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                if (owner_.view_ == View::Capture && ::PtInRect(&owner_.frame_, point) != FALSE &&
                    owner_.callbacks_.on_wheel_blocked) {
                    owner_.callbacks_.on_wheel_blocked();
                }
                return 0;
            }
            case WM_CTLCOLORSTATIC: {
                const HWND control = reinterpret_cast<HWND>(lparam);
                const COLORREF text = control == size_ ? theme::Current().text
                    : (warning_ ? theme::Current().error : theme::Current().muted_text);
                return ControlBackground(wparam, text);
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandlePreview(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
            case WM_NCDESTROY:
                preview_ = nullptr;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                break;
            case WM_NCHITTEST:
                return HTTRANSPARENT;  // Never takes the pointer or the wheel.
            case WM_SETCURSOR:
                return FALSE;
            case WM_ERASEBKGND:
                return 1;
            case theme::kChangedMessage:
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = ::BeginPaint(hwnd, &paint);
                RECT client{};
                ::GetClientRect(hwnd, &client);
                {
                    PaintBuffer buffer(dc, client);
                    PaintPreview(buffer.dc(), client, static_cast<float>(::GetDpiForWindow(hwnd)) / 96.0f);
                }
                ::EndPaint(hwnd, &paint);
                return 0;
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    void PaintPreview(HDC dc, const RECT& client, float scale) const {
        const auto& palette = theme::Current();
        theme::FillSolid(dc, client, palette.panel);
        theme::FrameSolid(dc, client, palette.control_border, theme::Scaled(1, scale));
        if (image_.width <= 0 || image_.height <= 0) { return; }
        const LONG padding = theme::Scaled(kPreviewPadding, scale);
        // The window shows the end of the image: the rows added last.
        const int rows = std::min<int>(image_.height, static_cast<int>(client.bottom - 2 * padding));
        if (rows <= 0) { return; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image_.width;
        info.bmiHeader.biHeight = -rows;  // top-down, only the rows that are drawn
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        const uint32_t* tail = image_.pixels.data() + static_cast<size_t>(image_.height - rows) * image_.width;
        ::SetDIBitsToDevice(dc, padding, padding, static_cast<DWORD>(image_.width), static_cast<DWORD>(rows),
                            0, 0, 0, static_cast<UINT>(rows), tail, &info, DIB_RGB_COLORS);
    }

    void LayoutPreview() {
        if (preview_ == nullptr) { return; }
        const bool show = owner_.visible_ && owner_.view_ == View::Capture && image_.height > 0 &&
                          !platform::IsEmptyRect(preview_area_);
        if (!show) {
            ::ShowWindow(preview_, SW_HIDE);
            return;
        }
        const float scale = platform::MonitorForRect(preview_area_).scale();
        const LONG padding = theme::Scaled(kPreviewPadding, scale);
        const LONG width = platform::RectWidth(preview_area_);
        const LONG height = std::min<LONG>(platform::RectHeight(preview_area_), image_.height + 2 * padding);
        RECT current{};
        ::GetWindowRect(preview_, &current);
        if (current.left != preview_area_.left || current.top != preview_area_.top ||
            platform::RectWidth(current) != width || platform::RectHeight(current) != height ||
            !::IsWindowVisible(preview_)) {
            ::SetWindowPos(preview_, HWND_TOPMOST, preview_area_.left, preview_area_.top, width, height,
                           SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }

    [[nodiscard]] HWND CreateControl(const wchar_t* class_name, const wchar_t* text, DWORD style, int id) const {
        HWND control = ::CreateWindowExW(0, class_name, text, WS_CHILD | style, 0, 0, 1, 1, island_,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         ::GetModuleHandleW(nullptr), nullptr);
        const bool button = ::lstrcmpW(class_name, L"BUTTON") == 0;
        if (control != nullptr && !(button ? StyleButton(control) : StyleLabel(control))) {
            ::DestroyWindow(control);
            return nullptr;
        }
        return control;
    }

    [[nodiscard]] bool CreateControls() {
        start_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_START), WS_TABSTOP | WS_GROUP | BS_DEFPUSHBUTTON, IDOK);
        help_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_HELP), WS_TABSTOP | BS_PUSHBUTTON, kIdHelp);
        size_ = CreateControl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, kIdSize);
        message_ = CreateControl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, kIdMessage);
        edit_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_EDIT), WS_TABSTOP | WS_GROUP | BS_PUSHBUTTON, kIdEdit);
        save_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_SAVE), WS_TABSTOP | BS_PUSHBUTTON, kIdSave);
        discard_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_DISCARD), WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
        // The default button, so Enter copies as it does in the editor.
        copy_ = CreateControl(L"BUTTON", strings::Get(IDS_SCROLLSHOT_COPY), WS_TABSTOP | BS_PUSHBUTTON, kIdCopyAlias);
        for (HWND control : {start_, help_, size_, message_, edit_, save_, discard_, copy_}) {
            if (control == nullptr) { return false; }
        }
        UpdateButtonStyle(start_, {Icon::Scissors, ButtonRole::Primary});
        UpdateButtonStyle(help_, {Icon::Help, ButtonRole::Standard, true});
        UpdateButtonStyle(edit_, {Icon::Pen, ButtonRole::Standard, true});
        UpdateButtonStyle(save_, {Icon::Save, ButtonRole::Standard, true});
        UpdateButtonStyle(discard_, {Icon::Discard, ButtonRole::Standard, true});
        UpdateButtonStyle(copy_, {Icon::Check, ButtonRole::Success, true});

        tooltip_ = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, island_, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (tooltip_ != nullptr) {
            const std::pair<HWND, UINT> tips[] = {
                {help_, IDS_SCROLLSHOT_HELP}, {edit_, IDS_SCROLLSHOT_EDIT}, {save_, IDS_SCROLLSHOT_SAVE},
                {discard_, IDS_SCROLLSHOT_DISCARD}, {copy_, IDS_SCROLLSHOT_COPY}};
            for (const auto& [control, id] : tips) {
                TOOLINFOW info{sizeof(info)};
                info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                info.hwnd = island_;
                info.uId = reinterpret_cast<UINT_PTR>(control);
                info.lpszText = const_cast<wchar_t*>(strings::Get(id));
                ::SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
            }
            StyleTooltip(tooltip_);
        }
        return true;
    }

    void RebuildFonts(float scale) {
        HFONT ui = theme::CreateUiFont(13.0f, scale, false);
        HFONT strong = theme::CreateUiFont(13.0f, scale, true);
        if (ui == nullptr || strong == nullptr) {
            if (ui != nullptr) { ::DeleteObject(ui); }
            if (strong != nullptr) { ::DeleteObject(strong); }
            return;
        }
        for (HWND control : {start_, help_, message_, edit_, save_, discard_, copy_}) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui), TRUE);
        }
        ::SendMessageW(size_, WM_SETFONT, reinterpret_cast<WPARAM>(strong), TRUE);
        if (ui_font_ != nullptr) { ::DeleteObject(ui_font_); }
        if (strong_font_ != nullptr) { ::DeleteObject(strong_font_); }
        ui_font_ = ui;
        strong_font_ = strong;
        scale_ = scale;
    }

    /// IDOK is taken by Start in the Start view; the handler maps it by view.
    static constexpr int kIdCopyAlias = IDOK;

    ScrollshotControls& owner_;
    HWND island_{nullptr};
    HWND preview_{nullptr};
    HWND tooltip_{nullptr};
    HWND start_{nullptr}, help_{nullptr}, size_{nullptr}, message_{nullptr};
    HWND edit_{nullptr}, save_{nullptr}, discard_{nullptr}, copy_{nullptr};
    HFONT ui_font_{nullptr};
    HFONT strong_font_{nullptr};
    float scale_{0.0f};
    RECT preview_area_{};
    scrollshot::ScrollshotPreview image_;
    bool warning_{false};
    bool user_positioned_{false};
};

ScrollshotControls::ScrollshotControls(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

ScrollshotControls::~ScrollshotControls() { Destroy(); }

bool ScrollshotControls::Show(View view, const RECT& frame, HWND owner) {
    if (platform::IsEmptyRect(frame)) { return false; }
    if (window_ == nullptr || window_->island() == nullptr) {
        window_ = std::make_unique<Window>(*this);
        if (!window_->Create(owner)) {
            window_.reset();
            return false;
        }
    }
    frame_ = frame;
    view_ = view;
    visible_ = true;
    window_->ApplyView();
    window_->Show();
    return true;
}

void ScrollshotControls::SetFrame(const RECT& frame) {
    if (window_ == nullptr || platform::IsEmptyRect(frame)) { return; }
    frame_ = frame;
    window_->Layout();
}

void ScrollshotControls::SetStatus(const std::wstring& size, const std::wstring& message, bool warning) {
    if (window_ != nullptr) { window_->SetStatus(size, message, warning); }
}

void ScrollshotControls::SetPreview(const scrollshot::ScrollshotPreview& preview) {
    if (window_ != nullptr) { window_->SetPreview(preview); }
}

void ScrollshotControls::SetBusy(bool busy) {
    if (window_ != nullptr) { window_->SetBusy(busy); }
}

void ScrollshotControls::Activate() {
    if (window_ != nullptr && visible_) { window_->Activate(); }
}

void ScrollshotControls::Hide() {
    visible_ = false;
    if (window_ != nullptr) { window_->Hide(); }
}

void ScrollshotControls::Destroy() {
    visible_ = false;
    window_.reset();
}

bool ScrollshotControls::ProcessDialogMessage(MSG& message) const {
    return window_ != nullptr && window_->ProcessDialogMessage(message);
}

HWND ScrollshotControls::hwnd() const noexcept { return window_ != nullptr ? window_->island() : nullptr; }
HWND ScrollshotControls::preview_hwnd() const noexcept { return window_ != nullptr ? window_->preview() : nullptr; }

int ScrollshotControls::PreviewPixelWidth(const RECT& frame) {
    const int wanted = theme::Scaled(kPreviewWidth, platform::MonitorForRect(frame).scale());
    return std::max(1, std::min<int>(wanted, static_cast<int>(platform::RectWidth(frame))));
}

}  // namespace easnap::ui
