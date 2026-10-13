#include "ui/native_controls.h"
#include <commctrl.h>
// Easnap - dimmed capture overlay implementation.
#include "ui/overlay.h"

#include "ui/island_layout.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>

#include "strings/strings.h"
#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {

constexpr wchar_t kDimClassName[] = L"EasnapOverlayDim";
constexpr wchar_t kChromeClassName[] = L"EasnapOverlayChrome";
constexpr wchar_t kImageClassName[] = L"EasnapCaptureImage";
constexpr wchar_t kMagnifierClassName[] = L"EasnapCaptureMagnifier";
constexpr wchar_t kModeToolbarClassName[] = L"EasnapCaptureModes";
constexpr UINT_PTR kNoticeTimerId = 1;
constexpr UINT_PTR kModeFadeTimerId = 2;
constexpr UINT kNoticeTimeoutMs = 2600;

struct PaletteButton {
    app::CaptureMode mode;
    const wchar_t* label;
    RECT rect;  ///< Client coordinates of the overlay window.
};

/// Order of the mode buttons, left to right.
constexpr std::array<app::CaptureMode, 3> kModes{
    app::CaptureMode::Screenshot, app::CaptureMode::Scrollshot, app::CaptureMode::Record};
constexpr int kFirstModeCommand = 3000;

/// Reusable GDI surface shared by one monitor's serial dim/chrome/magnifier paints.
class BackBuffer {
public:
    BackBuffer() = default;
    ~BackBuffer() {
        if (dc_ != nullptr) {
            if (previous_ != nullptr) { ::SelectObject(dc_, previous_); }
            if (bitmap_ != nullptr) { ::DeleteObject(bitmap_); }
            ::DeleteDC(dc_);
        }
    }

    BackBuffer(const BackBuffer&) = delete;
    BackBuffer& operator=(const BackBuffer&) = delete;


    template <typename Painter>
    void Paint(HWND window, Painter painter) {
        // BeginPaint consumes the update region. Preserve its disjoint strips:
        // rcPaint alone encloses the entire frame even for a one-pixel move.
        HRGN damage = ::CreateRectRgn(0, 0, 0, 0);
        const int region_type = damage ? ::GetUpdateRgn(window, damage, FALSE) : ERROR;
        PAINTSTRUCT ps{};
        HDC target = ::BeginPaint(window, &ps);
        if (target != nullptr && !::IsRectEmpty(&ps.rcPaint)) {
            RECT client{};
            ::GetClientRect(window, &client);
            if (Ensure(target, client.right, client.bottom)) {
                const int saved = ::SaveDC(dc_);
                if (saved != 0) {
                    if (region_type > NULLREGION) { ::SelectClipRgn(dc_, damage); }
                    ::IntersectClipRect(dc_, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right, ps.rcPaint.bottom);
                    painter(dc_, client);
                    ::BitBlt(target, ps.rcPaint.left, ps.rcPaint.top,
                        ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                        dc_, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                    ::RestoreDC(dc_, saved);
                } else {
                    painter(target, client);
                }
            } else {
                // GDI allocation failure still paints through the native clip.
                painter(target, client);
            }
        }
        ::EndPaint(window, &ps);
        if (damage != nullptr) { ::DeleteObject(damage); }
    }

private:
    bool Ensure(HDC target, int width, int height) {
        if (bitmap_ != nullptr && width <= width_ && height <= height_) { return true; }
        width = std::max(width, width_);
        height = std::max(height, height_);
        if (width <= 0 || height <= 0) { return false; }
        if (dc_ == nullptr) { dc_ = ::CreateCompatibleDC(target); }
        if (dc_ == nullptr) { return false; }
        HBITMAP next = ::CreateCompatibleBitmap(target, width, height);
        if (next == nullptr) { return false; }
        HGDIOBJ previous = ::SelectObject(dc_, next);
        if (previous == nullptr || previous == HGDI_ERROR) {
            ::DeleteObject(next);
            return false;
        }
        if (bitmap_ != nullptr) { ::DeleteObject(bitmap_); }
        else { previous_ = static_cast<HBITMAP>(previous); }
        bitmap_ = next;
        width_ = width;
        height_ = height;
        return true;
    }

    HDC dc_{nullptr};
    HBITMAP bitmap_{nullptr};
    HBITMAP previous_{nullptr};
    int width_{0};
    int height_{0};
};

/// Keeps one monitor's frozen desktop pixels in a GDI DIB so sub-rectangles can
/// be blitted straight into the overlay without touching Direct2D.
class FrozenBitmap {
public:
    FrozenBitmap() = default;
    ~FrozenBitmap() { Reset(); }

    FrozenBitmap(const FrozenBitmap&) = delete;
    FrozenBitmap& operator=(const FrozenBitmap&) = delete;

    bool Create(const platform::BgraImage& image) {
        Reset();
        if (image.empty() || image.pixels.size() !=
            static_cast<size_t>(image.width) * static_cast<size_t>(image.height)) {
            return false;
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;  // top-down
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        bitmap_ = ::CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap_ == nullptr || bits == nullptr) {
            Reset();
            return false;
        }
        std::memcpy(bits, image.pixels.data(), image.pixels.size() * sizeof(uint32_t));
        ::GdiFlush();

        dc_ = ::CreateCompatibleDC(nullptr);
        if (dc_ == nullptr) {
            Reset();
            return false;
        }
        previous_ = static_cast<HBITMAP>(::SelectObject(dc_, bitmap_));
        width_ = image.width;
        height_ = image.height;
        bits_ = static_cast<const uint32_t*>(bits);
        return true;
    }

    void Reset() {
        bits_ = nullptr;
        if (dc_ != nullptr) {
            if (previous_ != nullptr) {
                ::SelectObject(dc_, previous_);
                previous_ = nullptr;
            }
            ::DeleteDC(dc_);
            dc_ = nullptr;
        }
        if (bitmap_ != nullptr) {
            ::DeleteObject(bitmap_);
            bitmap_ = nullptr;
        }
        width_ = 0;
        height_ = 0;
    }

    [[nodiscard]] bool valid() const noexcept { return dc_ != nullptr && bitmap_ != nullptr; }

    [[nodiscard]] std::optional<COLORREF> Sample(const RECT& bounds, POINT cursor) const {
        if (!valid()) {
            return std::nullopt;
        }
        return SampleDesktopPixel(
            {bits_, static_cast<size_t>(width_) * static_cast<size_t>(height_)},
            width_, height_, bounds, cursor);
    }

    /// Copies `source` (bitmap coordinates) into `destination` (device context
    /// coordinates). Both rectangles have the same size.
    void Blit(HDC destination, const RECT& target_rect, POINT source_origin) const {
        if (!valid() || destination == nullptr) {
            return;
        }
        // Defensive clamp: a capture fallback may hand us a slightly different
        // resolution than the monitor reports.
        const int source_x = std::clamp(static_cast<int>(source_origin.x), 0, width_);
        const int source_y = std::clamp(static_cast<int>(source_origin.y), 0, height_);
        const int width = std::min(static_cast<int>(platform::RectWidth(target_rect)), width_ - source_x);
        const int height = std::min(static_cast<int>(platform::RectHeight(target_rect)), height_ - source_y);
        if (width <= 0 || height <= 0) {
            return;
        }
        ::BitBlt(destination, target_rect.left, target_rect.top, width, height, dc_, source_x, source_y, SRCCOPY);
    }

private:
    const uint32_t* bits_{nullptr};  // Owned by bitmap_, immutable after Create.
    HDC dc_{nullptr};
    HBITMAP bitmap_{nullptr};
    HBITMAP previous_{nullptr};
    int width_{0};
    int height_{0};
};

}  // namespace

/// A dimming window plus its opaque chrome companion, covering one monitor.
class OverlayWindow {
public:
    OverlayWindow(OverlayController& controller, const platform::MonitorInfo& monitor, bool owns_palette)
        : controller_(controller), monitor_(monitor), owns_palette_(owns_palette) {}

    ~OverlayWindow() { Destroy(); }

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    bool Create(const capture::MonitorFrame* frame) {
        if (!EnsureWindowClasses()) {
            return false;
        }
        const int width = static_cast<int>(platform::RectWidth(monitor_.bounds));
        const int height = static_cast<int>(platform::RectHeight(monitor_.bounds));
        const int left = static_cast<int>(monitor_.bounds.left);
        const int top = static_cast<int>(monitor_.bounds.top);
        HINSTANCE instance = ::GetModuleHandleW(nullptr);

        dim_ = ::CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kDimClassName,
                                 strings::Get(IDS_SHELL_OVERLAY_WINDOW), WS_POPUP, left, top, width, height, nullptr,
                                 nullptr, instance, this);
        if (dim_ == nullptr) {
            platform::LogMessage(L"CreateWindowEx(overlay dim) failed (error %lu)", ::GetLastError());
            return false;
        }
        // A nearly transparent input plane keeps clear live pixels clickable.
        // Dimming belongs to a separate clipped window outside the selection.
        if (::SetLayeredWindowAttributes(dim_, 0, 1, LWA_ALPHA) == FALSE) {
            platform::LogMessage(L"SetLayeredWindowAttributes(dim) failed (error %lu)", ::GetLastError());
        }

        shade_ = ::CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_NOACTIVATE|WS_EX_TOPMOST|WS_EX_TOOLWINDOW,
            kChromeClassName,strings::Get(IDS_SHELL_OVERLAY_SHADE_WINDOW),WS_POPUP,left,top,width,height,dim_,nullptr,instance,this);
        if(!shade_ || !::SetLayeredWindowAttributes(shade_,0,0,LWA_ALPHA)) { Destroy(); return false; }

        image_ = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kImageClassName, strings::Get(IDS_SHELL_OVERLAY_IMAGE_WINDOW), WS_POPUP, left, top, 1, 1, shade_, nullptr,
            instance, this);
        if (!image_ || !::SetLayeredWindowAttributes(image_,0,0,LWA_ALPHA)) {
            Destroy();
            return false;
        }

        // Ownership keeps chrome above the inline image, which is above dimming.
        chrome_ = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kChromeClassName, strings::Get(IDS_SHELL_OVERLAY_CHROME_WINDOW), WS_POPUP, left, top, width, height, image_,
            nullptr, instance, this);
        if (chrome_ == nullptr) {
            platform::LogMessage(L"CreateWindowEx(overlay chrome) failed (error %lu)", ::GetLastError());
            Destroy();
            return false;
        }
        if (::SetLayeredWindowAttributes(chrome_, theme::kChromeColorKey, 255, LWA_COLORKEY) == FALSE) {
            platform::LogMessage(L"SetLayeredWindowAttributes(chrome) failed (error %lu)", ::GetLastError());
        }

        label_font_ = theme::CreateUiFont(13.0f, monitor_.scale(), false);
        button_font_ = theme::CreateUiFont(15.0f, monitor_.scale(), true);
        MeasureModeButtons();
        if (owns_palette_) {
            const RECT panel = ToolbarPanel();
            mode_toolbar_ = ::CreateWindowExW(
                WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT,
                kModeToolbarClassName, strings::Get(IDS_SHELL_OVERLAY_MODES_WINDOW), WS_POPUP | WS_CLIPCHILDREN,
                left + panel.left, top + panel.top, panel.right - panel.left, panel.bottom - panel.top,
                chrome_, nullptr, instance, this);
            if (!mode_toolbar_ || !::SetLayeredWindowAttributes(mode_toolbar_, theme::kChromeColorKey,
                    255, LWA_COLORKEY | LWA_ALPHA)) {
                Destroy();
                return false;
            }
            ::GetWindowRect(mode_toolbar_, &mode_home_);
        }

        magnifier_ = ::CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kMagnifierClassName, strings::Get(IDS_SHELL_OVERLAY_MAGNIFIER_WINDOW), WS_POPUP, left, top, 1, 1, dim_,
            nullptr, instance, this);
        if (magnifier_ == nullptr ||
            !::SetLayeredWindowAttributes(magnifier_, 0, 255, LWA_ALPHA)) {
            Destroy();
            return false;
        }

        if (frame != nullptr) {
            frozen_.Create(frame->image);
        }
        if (mode_toolbar_) {
            constexpr std::array<Icon, 3> icons{Icon::Rectangle, Icon::Scissors, Icon::Record};
            const auto buttons = PaletteLayout(true);
            for (size_t i = 0; i < buttons.size(); ++i) {
                const auto& r = buttons[i].rect;
                mode_buttons_[i] = ::CreateWindowExW(0, L"BUTTON", buttons[i].label,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_RADIOBUTTON | BS_PUSHLIKE | (i == 0 ? WS_GROUP : 0),
                    r.left, r.top, r.right-r.left, r.bottom-r.top, mode_toolbar_,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(kFirstModeCommand + static_cast<int>(i))), instance, nullptr);
                ButtonStyle style; style.icon=icons[i]; style.preserve_disabled_visual=true; style.hide_focus_line=true; style.segmented=true;
                if (!mode_buttons_[i] || !StyleButton(mode_buttons_[i], style)) {
                    Destroy(); return false;
                }
                ::SendMessageW(mode_buttons_[i], WM_SETFONT, reinterpret_cast<WPARAM>(button_font_), TRUE);
            }
            if (!CaptureExclusionSupported()) {
                // The reason for the disabled button is its accessible name too.
                ::SetWindowTextW(mode_buttons_[1], strings::Get(IDS_MODE_SCROLLSHOT_UNSUPPORTED));
            }
            static_cast<void>(::SetWindowDisplayAffinity(mode_toolbar_, 0x11));
            SyncModeButtons();
        }

        for(HWND window:{dim_,shade_,image_,chrome_,mode_toolbar_,magnifier_}) { if(window) { ::SetWindowDisplayAffinity(window,0x11); } }
        if(mode_toolbar_) { RoundWindow(mode_toolbar_); }
        // Allocate and fill the static shade before interaction. With an
        // implicit full-monitor selection its initial region is empty; letting
        // the first drag allocate/repaint it stalls that input on larger screens.
        // Every later selection changes only the compositor's clipping mask.
        ::ShowWindow(shade_, SW_SHOWNOACTIVATE);
        if (HDC shade_dc = ::GetDC(shade_)) {
            theme::FillSolid(shade_dc, {0,0,width,height}, theme::kDim);
            ::ReleaseDC(shade_, shade_dc);
        } else { Destroy(); return false; }
        ::GdiFlush();
        UpdateShade();
        ::SetLayeredWindowAttributes(shade_,0,theme::kDimAlpha,LWA_ALPHA);
        ::ShowWindow(dim_, SW_SHOWNOACTIVATE);
        ::ShowWindow(chrome_, SW_SHOWNOACTIVATE);
        ::UpdateWindow(dim_);
        ::UpdateWindow(chrome_);
        if (mode_toolbar_) {
            ::ShowWindow(mode_toolbar_, SW_SHOWNOACTIVATE);
            ::UpdateWindow(mode_toolbar_);
        }
        return true;
    }

    void Activate() const {
        if (dim_ == nullptr) {
            return;
        }
        ::SetForegroundWindow(dim_);
        ::SetFocus(dim_);
    }

    void Hide() const {
        ::KillTimer(dim_, kModeFadeTimerId);
        if (mode_toolbar_) { ::ShowWindow(mode_toolbar_, SW_HIDE); }
        ::ShowWindow(image_, SW_HIDE);
        if(shade_) { ::ShowWindow(shade_,SW_HIDE); }
        HideShadeStrips();
        if (magnifier_ != nullptr) {
            if (::IsWindowVisible(magnifier_)) { ::ShowWindow(magnifier_, SW_HIDE); }
        }
        if (chrome_ != nullptr) {
            ::ShowWindow(chrome_, SW_HIDE);
        }
        if (dim_ != nullptr) {
            ::ShowWindow(dim_, SW_HIDE);
        }
    }

    void Resume() const {
        UpdateShade();
        UpdateInputRegion();
        ::ShowWindow(dim_, SW_SHOWNOACTIVATE);
        ::ShowWindow(chrome_, SW_SHOWNOACTIVATE);
        ArmModeFade();
        Invalidate();
    }

    void BeginModeFade() {
        SyncModeButtons();
        if (!mode_toolbar_ || mode_fade_start_) { return; }
        mode_fade_start_ = ::GetTickCount64();
        if (controller_.visible()) { ArmModeFade(); }
    }

    void FinishModeFade() const {
        ::KillTimer(dim_, kModeFadeTimerId);
        ::ShowWindow(mode_toolbar_, SW_HIDE);
    }

    bool UpdateModeToolbar() const {
        if (!mode_toolbar_ || !controller_.visible()) { return false; }
        const BYTE alpha = mode_fade_start_ ? ModeToolbarOpacity(::GetTickCount64() - *mode_fade_start_) : 255;
        if (alpha == 0 || !::SetLayeredWindowAttributes(mode_toolbar_, theme::kChromeColorKey,
                alpha, LWA_COLORKEY | LWA_ALPHA)) {
            FinishModeFade();
            return false;
        }
        // While the frame is dragged the island stays away; the fade goes on.
        if (!mode_suppressed_ && !::IsWindowVisible(mode_toolbar_)) { ::ShowWindow(mode_toolbar_, SW_SHOWNOACTIVATE); }
        return mode_fade_start_.has_value();
    }

    /// The mode island in desktop pixels; empty on a monitor that has none.
    [[nodiscard]] RECT ModeToolbarRect() const {
        RECT rect{};
        if (mode_toolbar_ != nullptr) { ::GetWindowRect(mode_toolbar_, &rect); }
        return rect;
    }

    /// Puts the mode island where the frame leaves room for it on the top
    /// edge: at its home, or beside the frame when the frame takes that place.
    void PlaceModeToolbar() const {
        if (mode_toolbar_ == nullptr) { return; }
        const RegionSelector& selector = controller_.selector();
        const RECT frame = selector.has_selection() ? selector.selection() : RECT{};
        const RECT target = PlaceModeIsland(mode_home_, frame, monitor_.work_area.left, monitor_.work_area.right,
                                            theme::Scaled(8, monitor_.scale()));
        RECT current{};
        ::GetWindowRect(mode_toolbar_, &current);
        if (::EqualRect(&current, &target) == FALSE) {
            ::SetWindowPos(mode_toolbar_, nullptr, target.left, target.top, 0, 0,
                           SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    /// The frame is being dragged or resized: the mode island steps aside,
    /// like the tools island, and returns in its place for the new frame.
    void SetModeToolbarSuppressed(bool suppressed) {
        if (mode_toolbar_ == nullptr) { return; }
        if (!suppressed) { PlaceModeToolbar(); }
        if (mode_suppressed_ == suppressed) { return; }
        mode_suppressed_ = suppressed;
        if (suppressed) {
            // A hidden window must not keep the keyboard.
            const HWND focus = ::GetFocus();
            if (focus != nullptr && (focus == mode_toolbar_ || ::IsChild(mode_toolbar_, focus) != FALSE)) { ::SetFocus(dim_); }
            ::ShowWindow(mode_toolbar_, SW_HIDE);
        } else if (controller_.visible()) {
            ArmModeFade();
        }
    }

    void ArmModeFade() const {
        if (UpdateModeToolbar() && !::SetTimer(dim_, kModeFadeTimerId, 16, nullptr)) { FinishModeFade(); }
    }

    void Destroy() {
        if (dim_) { ::KillTimer(dim_, kModeFadeTimerId); }
        if (mode_toolbar_) {
            ::SetWindowLongPtrW(mode_toolbar_, GWLP_USERDATA, 0);
            ::DestroyWindow(mode_toolbar_);
            mode_toolbar_ = nullptr;
            mode_buttons_ = {};
        }
        frozen_.Reset();
        if (label_font_ != nullptr) {
            ::DeleteObject(label_font_);
            label_font_ = nullptr;
        }
        if (button_font_ != nullptr) {
            ::DeleteObject(button_font_);
            button_font_ = nullptr;
        }
        if (magnifier_ != nullptr) {
            ::SetWindowLongPtrW(magnifier_, GWLP_USERDATA, 0);
            ::DestroyWindow(magnifier_);
            magnifier_ = nullptr;
        }
        if (chrome_ != nullptr) {
            ::SetWindowLongPtrW(chrome_, GWLP_USERDATA, 0);
            ::DestroyWindow(chrome_);
            chrome_ = nullptr;
        }
        if (image_ != nullptr) {
            ::SetWindowLongPtrW(image_, GWLP_USERDATA, 0);
            ::DestroyWindow(image_);
            image_ = nullptr;
        }
        DestroyShadeStrips();
        if(shade_) { ::DestroyWindow(shade_); shade_=nullptr; }
        if (dim_ != nullptr) {
            ::SetWindowLongPtrW(dim_, GWLP_USERDATA, 0);
            ::DestroyWindow(dim_);
            dim_ = nullptr;
        }
    }

    void Invalidate() const {
        SyncModeButtons();
        UpdateShade();
        UpdateInputRegion();
        UpdateImage();
        if (mode_toolbar_) { ::InvalidateRect(mode_toolbar_, nullptr, FALSE); }
        if (dim_ != nullptr) {
            ::InvalidateRect(dim_, nullptr, FALSE);
        }
        if (chrome_ != nullptr) {
            ::InvalidateRect(chrome_, nullptr, FALSE);
        }
    }

    void InvalidateSelection(const RECT& previous, bool previous_has_selection) const {
        UpdateShade();
        const RECT current = controller_.selector().selection();
        for (const RECT& selection : {previous, current}) {
            for (const RECT& damage : RegionSelector::GdiSelectionDamage(selection, monitor_.bounds, monitor_.scale())) {
                if (!::IsRectEmpty(&damage)) { ::InvalidateRect(chrome_, &damage, FALSE); }
            }

        }
        // A sub-minimum creation uses monitor-wide crosshairs instead of a frame.
        if (!previous_has_selection || !controller_.selector().has_selection()) {
            ::InvalidateRect(chrome_, nullptr, FALSE);
        }
    }

    void InvalidateImage(const RECT& damage) const {
        if (!controller_.visible() || !controller_.workspace().screenshot_fixed()) { return; }
        RECT bounds{};
        ::GetWindowRect(image_, &bounds);
        RECT local = platform::IntersectRects(damage, bounds);
        if (platform::IsEmptyRect(local)) { return; }
        ::OffsetRect(&local, -bounds.left, -bounds.top);
        ::InvalidateRect(image_, &local, FALSE);
    }

    void SyncModeButtons() const {
        for (size_t i = 0; i < mode_buttons_.size(); ++i) {
            HWND button = mode_buttons_[i];
            if (!button) { continue; }
            // Scrollshot needs capture exclusion; without it the button stays off.
            const bool enabled = controller_.workspace().can_switch_mode() &&
                (kModes[i] != app::CaptureMode::Scrollshot || CaptureExclusionSupported());
            if ((::IsWindowEnabled(button) != FALSE) != enabled) { ::EnableWindow(button, enabled); }
            const WPARAM checked = kModes[i] == controller_.mode() ? BST_CHECKED : BST_UNCHECKED;
            if (::SendMessageW(button, BM_GETCHECK, 0, 0) != static_cast<LRESULT>(checked)) { ::SendMessageW(button, BM_SETCHECK, checked, 0); }
        }
    }

    bool ProcessDialogMessage(MSG& message) const {
        if (!mode_toolbar_ || !::IsWindowVisible(mode_toolbar_) ||
            !(message.hwnd == mode_toolbar_ || ::IsChild(mode_toolbar_, message.hwnd))) { return false; }
        if (ConsumePopupInput(message.message, message.wParam)) { return true; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { controller_.HandleCancelled(); return true; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
            const bool backwards = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if ((!backwards && message.hwnd == mode_buttons_.back()) || (backwards && message.hwnd == mode_buttons_.front())) {
                if (controller_.workspace().screenshot_visible()) { controller_.EditorKey(WM_KEYDOWN, VK_TAB, 0); }
                else { ::SetForegroundWindow(dim_); ::SetFocus(dim_); }
                return true;
            }
        }
        return ActivateFocusedButton(message, mode_toolbar_) || ::IsDialogMessageW(mode_toolbar_, &message) != FALSE;
    }

    [[nodiscard]] HWND hwnd() const noexcept { return dim_; }
    [[nodiscard]] HWND panel_owner() const noexcept { return chrome_; }
    [[nodiscard]] const platform::MonitorInfo& monitor() const noexcept { return monitor_; }
    [[nodiscard]] bool owns_palette() const noexcept { return owns_palette_; }

    [[nodiscard]] std::optional<COLORREF> SamplePixel(POINT cursor) const {
        return frozen_.Sample(monitor_.bounds, cursor);
    }

    [[nodiscard]] bool MagnifierContains(POINT point) const {
        RECT box{};
        return ::IsWindowVisible(magnifier_) && ::GetWindowRect(magnifier_, &box) && ::PtInRect(&box, point);
    }

    /// See OverlayController::ExposeScrollshotFrame. Without the four windows
    /// the shading stays as it is and the capture is merely not exact.
    void SetFrameExposed(bool exposed) {
        if (exposed && shade_ && !shade_strips_[0]) {
            HINSTANCE instance = ::GetModuleHandleW(nullptr);
            for (HWND& strip : shade_strips_) {
                strip = ::CreateWindowExW(
                    WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    kChromeClassName, strings::Get(IDS_SHELL_OVERLAY_SHADE_WINDOW), WS_POPUP, monitor_.bounds.left,
                    monitor_.bounds.top, 1, 1, dim_, nullptr, instance, this);
                if (!strip || !::SetLayeredWindowAttributes(strip, 0, theme::kDimAlpha, LWA_ALPHA) ||
                    !::SetWindowDisplayAffinity(strip, 0x11)) {
                    platform::LogMessage(L"The outside shading could not be split (error %lu)", ::GetLastError());
                    DestroyShadeStrips();
                    break;
                }
            }
        }
        UpdateShade();
    }

    [[nodiscard]] bool IsShadeStrip(HWND hwnd) const noexcept {
        return hwnd != nullptr && std::find(shade_strips_.begin(), shade_strips_.end(), hwnd) != shade_strips_.end();
    }

    void HideShadeStrips() const {
        for (HWND strip : shade_strips_) {
            if (strip && ::IsWindowVisible(strip)) { ::ShowWindow(strip, SW_HIDE); }
        }
    }

    void DestroyShadeStrips() noexcept {
        for (HWND& strip : shade_strips_) {
            if (strip) { ::SetWindowLongPtrW(strip, GWLP_USERDATA, 0); ::DestroyWindow(strip); strip = nullptr; }
        }
    }

    void UpdateShade() const {
        if(!shade_) { return; }
        const RECT selection=platform::IntersectRects(controller_.selector().selection(),monitor_.bounds);
        if (controller_.frame_exposed() && shade_strips_[0] && controller_.selector().has_selection() &&
            !platform::IsEmptyRect(selection)) {
            // Above, below, left and right of the frame: none contains it.
            const RECT& monitor = monitor_.bounds;
            const std::array<RECT, 4> strips{{{monitor.left, monitor.top, monitor.right, selection.top},
                                              {monitor.left, selection.bottom, monitor.right, monitor.bottom},
                                              {monitor.left, selection.top, selection.left, selection.bottom},
                                              {selection.right, selection.top, monitor.right, selection.bottom}}};
            for (size_t i = 0; i < strips.size(); ++i) {
                HWND strip = shade_strips_[i];
                if (platform::IsEmptyRect(strips[i])) {
                    if (::IsWindowVisible(strip)) { ::ShowWindow(strip, SW_HIDE); }
                    continue;
                }
                RECT current{};
                ::GetWindowRect(strip, &current);
                if (::IsWindowVisible(strip) && ::EqualRect(&current, &strips[i])) { continue; }
                // Below the image and chrome windows, like the shading it replaces.
                ::SetWindowPos(strip, image_, strips[i].left, strips[i].top, platform::RectWidth(strips[i]),
                               platform::RectHeight(strips[i]), SWP_NOACTIVATE | SWP_SHOWWINDOW);
                ::RedrawWindow(strip, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
            }
            if (::IsWindowVisible(shade_)) { ::ShowWindow(shade_, SW_HIDE); }
            return;
        }
        HRGN outside=::CreateRectRgn(0,0,client_width(),client_height());
        if(controller_.selector().has_selection() && !platform::IsEmptyRect(selection)) {
            HRGN inside=::CreateRectRgn(selection.left-monitor_.bounds.left,selection.top-monitor_.bounds.top,
                selection.right-monitor_.bounds.left,selection.bottom-monitor_.bounds.top);
            ::CombineRgn(outside,outside,inside,RGN_DIFF); ::DeleteObject(inside);
        }
        HRGN previous=::CreateRectRgn(0,0,0,0);
        const bool same=::GetWindowRgn(shade_,previous)!=ERROR && ::EqualRgn(previous,outside);
        ::DeleteObject(previous);
        if(same || !::SetWindowRgn(shade_,outside,FALSE)) { ::DeleteObject(outside); }
        if(!::IsWindowVisible(shade_)) { ::ShowWindow(shade_,SW_SHOWNOACTIVATE); }
        HideShadeStrips();
    }

    /// The input plane covers the whole monitor, except while a Scrollshot is
    /// capturing: then it leaves the frame open, so the wheel and the pointer
    /// reach the application underneath and everything outside stays inert.
    void UpdateInputRegion() const {
        if (!dim_) { return; }
        const RECT selection = platform::IntersectRects(controller_.selector().selection(), monitor_.bounds);
        const bool open = controller_.workspace().scrollshot_capturing() && !platform::IsEmptyRect(selection);
        HRGN wanted = nullptr;
        if (open) {
            wanted = ::CreateRectRgn(0, 0, client_width(), client_height());
            HRGN inside = ::CreateRectRgn(selection.left - monitor_.bounds.left, selection.top - monitor_.bounds.top,
                selection.right - monitor_.bounds.left, selection.bottom - monitor_.bounds.top);
            if (wanted && inside) { ::CombineRgn(wanted, wanted, inside, RGN_DIFF); }
            if (inside) { ::DeleteObject(inside); }
        }
        HRGN current = ::CreateRectRgn(0, 0, 0, 0);
        const int current_type = current ? ::GetWindowRgn(dim_, current) : ERROR;
        const bool same = open ? (current_type != ERROR && wanted && ::EqualRgn(current, wanted)) : current_type == ERROR;
        if (current) { ::DeleteObject(current); }
        // The system owns a region it accepted; ours is deleted only when unused.
        if (same || !::SetWindowRgn(dim_, wanted, FALSE)) { if (wanted) { ::DeleteObject(wanted); } }
    }

    void UpdateImage() const {
        if (!controller_.visible() || !controller_.workspace().screenshot_fixed()) {
            ::ShowWindow(image_, SW_HIDE);
            return;
        }
        const RECT box = platform::IntersectRects(controller_.selector().selection(), monitor_.bounds);
        if (platform::IsEmptyRect(box)) { ::ShowWindow(image_, SW_HIDE); return; }
        ::SetWindowPos(image_,nullptr,box.left,box.top,box.right-box.left,box.bottom-box.top,SWP_NOZORDER|SWP_NOACTIVATE);
        if(!::IsWindowVisible(image_)) {
            // USER32 allocates this redirected surface on ShowWindow. Painting
            // a hidden HWND loses its first frame. Show transparently, finish the
            // real paint, then expose it; keep display-affinity capture exclusion.
            ::SetLayeredWindowAttributes(image_,0,0,LWA_ALPHA);
            ::ShowWindow(image_,SW_SHOWNOACTIVATE);
            ::RedrawWindow(image_,nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW);
            if(HDC dc=::GetDC(image_)) {
                const RECT client{0,0,box.right-box.left,box.bottom-box.top};
                controller_.PaintImage(dc,box,client); ::ReleaseDC(image_,dc);
            }
            ::GdiFlush();
            ::SetLayeredWindowAttributes(image_,0,255,LWA_ALPHA);
        } else { ::InvalidateRect(image_,nullptr,FALSE); }
    }

    void UpdateMagnifier() const {
        if (!controller_.visible() || !controller_.workspace().magnifier_visible() || !::PtInRect(&monitor_.bounds, controller_.pointer())) {
            if (::IsWindowVisible(magnifier_)) { ::ShowWindow(magnifier_, SW_HIDE); }
            return;
        }
        const float scale = monitor_.scale();
        const LONG padding = theme::Scaled(theme::kMagnifierPadding, scale);
        const LONG grid = theme::kMagnifierGridPixels * theme::Scaled(theme::kMagnifierCell, scale);
        const SIZE size{theme::Scaled(theme::kMagnifierWidth, scale),
            grid + padding * 3 + theme::Scaled(theme::kMagnifierLineHeight, scale) * 3};
        RECT toolbar = ToolbarPanel();
        if (owns_palette_) {
            ::OffsetRect(&toolbar, monitor_.bounds.left, monitor_.bounds.top);
        } else {
            toolbar = {};
        }
        const RECT box = PlaceCaptureMagnifier(controller_.pointer(), monitor_.bounds, size,
            theme::Scaled(theme::kMagnifierPointerGap, scale), toolbar);
        RECT previous{};
        ::GetClientRect(magnifier_, &previous);
        ::SetWindowPos(magnifier_, HWND_TOPMOST, box.left, box.top,
            box.right - box.left, box.bottom - box.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        if (previous.right != box.right - box.left || previous.bottom != box.bottom - box.top) {
            RoundWindow(magnifier_, 8.0f);
        }
        ::InvalidateRect(magnifier_, nullptr, FALSE);
    }

private:
    // -----------------------------------------------------------------------
    // Window plumbing
    // -----------------------------------------------------------------------

    static bool EnsureWindowClasses() {
        static const bool registered = [] {
            WNDCLASSEXW dim{};
            dim.cbSize = sizeof(dim);
            dim.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
            dim.lpfnWndProc = &OverlayWindow::DimWindowProc;
            dim.hInstance = ::GetModuleHandleW(nullptr);
            dim.hCursor = nullptr;  // The selector decides which cursor to show.
            dim.lpszClassName = kDimClassName;

            WNDCLASSEXW chrome{};
            chrome.cbSize = sizeof(chrome);
            chrome.style = CS_HREDRAW | CS_VREDRAW;
            chrome.lpfnWndProc = &OverlayWindow::ChromeWindowProc;
            chrome.hInstance = ::GetModuleHandleW(nullptr);
            chrome.hCursor = nullptr;
            chrome.lpszClassName = kChromeClassName;

            WNDCLASSEXW image = chrome;
            image.lpszClassName = kImageClassName;
            WNDCLASSEXW magnifier = chrome;
            magnifier.lpszClassName = kMagnifierClassName;
            WNDCLASSEXW modes = chrome;
            modes.lpszClassName = kModeToolbarClassName;
            return ::RegisterClassExW(&dim) != 0 && ::RegisterClassExW(&chrome) != 0 &&
                   ::RegisterClassExW(&magnifier) != 0 && ::RegisterClassExW(&image) != 0 &&
                   ::RegisterClassExW(&modes) != 0;
        }();
        return registered;
    }

    static OverlayWindow* FromHandle(HWND hwnd, UINT message, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        return reinterpret_cast<OverlayWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    static LRESULT CALLBACK DimWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (OverlayWindow* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandleDimMessage(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    static LRESULT CALLBACK ChromeWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (OverlayWindow* self = FromHandle(hwnd, message, lparam); self != nullptr) {
            return self->HandleChromeMessage(hwnd, message, wparam, lparam);
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    [[nodiscard]] POINT ToVirtual(LPARAM lparam) const {
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        pt.x += monitor_.bounds.left;
        pt.y += monitor_.bounds.top;
        return pt;
    }

    [[nodiscard]] int client_width() const { return static_cast<int>(platform::RectWidth(monitor_.bounds)); }
    [[nodiscard]] int client_height() const { return static_cast<int>(platform::RectHeight(monitor_.bounds)); }

    LRESULT HandleDimMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (ConsumePopupInput(message, wparam)) { return 0; }
        switch (message) {
            case WM_MOUSEACTIVATE:
                if (controller_.workspace().recording_visible()) { return MA_NOACTIVATE; }
                break;
            case theme::kChangedMessage:
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                buffer_.Paint(hwnd, [this](HDC dc, const RECT&) { PaintDim(dc); });
                return 0;
            }

            case WM_SETCURSOR: {
                if (controller_.workspace().scrollshot_locked()) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
                    return TRUE;
                }
                POINT pt{};
                ::GetCursorPos(&pt);
                const auto annotation_cursor=controller_.AnnotationCursor(pt);
                if(annotation_cursor && !ToolbarContains(pt) && !controller_.MagnifierContains(pt)) { ::SetCursor(annotation_cursor); return TRUE; }
                const LPCWSTR cursor = ToolbarContains(pt) || controller_.MagnifierContains(pt) ? IDC_ARROW :
                    (controller_.UsesAnnotationTool() && controller_.selector().HitTest(pt) == SelectionHandle::Body
                        ? IDC_CROSS : (controller_.workspace().screenshot_fixed() ? IDC_ARROW : controller_.selector().CursorForPoint(pt)));
                ::SetCursor(::LoadCursorW(nullptr, cursor));
                return TRUE;
            }

            case WM_MOUSEMOVE: {
                const POINT pt = ToVirtual(lparam);
                controller_.UpdatePointer(pt);
                controller_.PointerMove(pt, (wparam & MK_LBUTTON) != 0);
                const int hovered = PaletteHitTest(pt);
                if (hovered != hover_button_) {
                    hover_button_ = hovered;
                    if (mode_toolbar_) { ::InvalidateRect(mode_toolbar_, nullptr, FALSE); }
                }
                return 0;
            }
            case WM_LBUTTONDOWN:
            case WM_LBUTTONDBLCLK: {
                controller_.color_readout().CancelPress();
                const POINT pt = ToVirtual(lparam);
                const bool toolbar = ToolbarContains(pt);
                const bool excluded = toolbar || controller_.MagnifierContains(pt);
                controller_.UpdatePointer(pt);
                if (!controller_.workspace().recording_visible()) { ::SetFocus(hwnd); }
                controller_.PointerDown(pt, message == WM_LBUTTONDBLCLK, excluded,
                    static_cast<DWORD>(::GetMessageTime()));
                ::SetCapture(hwnd);
                if (toolbar) { OnPaletteClicked(PaletteHitTest(pt)); }
                return 0;
            }
            case WM_LBUTTONUP:
                controller_.PointerUp(ToVirtual(lparam));
                if (::GetCapture() == hwnd) { ::ReleaseCapture(); }
                return 0;

            case WM_RBUTTONUP:
                // A stray right-click must not throw a stitched image away.
                if (!controller_.workspace().scrollshot_locked()) { controller_.HandleCancelled(); }
                return 0;

            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL: {
                const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                const RECT frame = controller_.selector().selection();
                if (controller_.workspace().scrollshot_capturing()) {
                    // With "scroll inactive windows" off, Windows sends the wheel
                    // here instead of to the content under the pointer.
                    if (::PtInRect(&frame, pt)) { controller_.RequestScrollshotCommand(ScrollshotCommand::WheelBlocked); }
                } else if (controller_.workspace().screenshot_fixed()) {
                    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
                    const WORD keys = GET_KEYSTATE_WPARAM(wparam);
                    const bool horizontal = message == WM_MOUSEHWHEEL;
                    controller_.EditorWheel(horizontal ? -delta : delta, (keys & MK_CONTROL) != 0,
                                            horizontal || (keys & MK_SHIFT) != 0, pt);
                }
                return 0;
            }

            case WM_CHAR:
                if (controller_.workspace().screenshot_visible()) {
                    controller_.EditorKey(message, wparam, lparam);
                }
                return 0;

            case WM_KEYDOWN:
                if (controller_.workspace().scrollshot_locked()) {
                    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (wparam == VK_ESCAPE) { controller_.RequestScrollshotCommand(ScrollshotCommand::Discard); }
                    else if (wparam == VK_RETURN || (control && wparam == 'C')) { controller_.RequestScrollshotCommand(ScrollshotCommand::Copy); }
                    else if (control && wparam == 'S') { controller_.RequestScrollshotCommand(ScrollshotCommand::Save); }
                    return 0;
                }
                if (wparam == VK_TAB && mode_toolbar_ && controller_.workspace().can_switch_mode()) {
                    ::SetForegroundWindow(mode_toolbar_); ::SetFocus(mode_buttons_[0]); return 0;
                }
                if (controller_.workspace().screenshot_visible()) {
                    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    if (controller_.workspace().screenshot_fixed() || (!(wparam >= VK_LEFT && wparam <= VK_DOWN) && !(control && wparam == 'A'))) {
                        controller_.EditorKey(message, wparam, lparam);
                        return 0;
                    }
                }
                controller_.color_readout().OnKeyDown(wparam, (lparam & (1LL << 30)) != 0,
                    (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
                    (::GetKeyState(VK_MENU) & 0x8000) != 0);
                return HandleKey(wparam);

            case WM_KEYUP:
                if (controller_.workspace().magnifier_visible() && controller_.color_readout().OnKeyUp(wparam)) {
                    controller_.UpdatePointer(controller_.pointer());
                }
                return 0;

            case WM_SYSKEYDOWN:
            case WM_KILLFOCUS:
                controller_.color_readout().CancelPress();
                break;

            case WM_CAPTURECHANGED:
                controller_.CancelPointer();
                return 0;

            case WM_TIMER:
                if (wparam == kModeFadeTimerId) { UpdateModeToolbar(); return 0; }
                if (wparam == kNoticeTimerId) {
                    ::KillTimer(hwnd, kNoticeTimerId);
                    controller_.SetNotice({});
                }
                return 0;

            case WM_DISPLAYCHANGE:
                // A locked Scrollshot holds captured content: the coordinator
                // stops it and keeps the image instead of cancelling here.
                if (!controller_.workspace().scrollshot_locked()) { controller_.HandleCancelled(); }
                return 0;

            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandleChromeMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (ConsumePopupInput(message, wparam)) { return 0; }
        switch (message) {
            case WM_NCLBUTTONDBLCLK: if(hwnd==mode_toolbar_) { return 0; } break;
            case WM_EXITSIZEMOVE:
                // Where the user left it is its place from now on.
                if(hwnd==mode_toolbar_) { ::GetWindowRect(hwnd,&mode_home_); controller_.ModeIslandMoved(); return 0; }
                break;
            case WM_MOVING:
                if(hwnd==mode_toolbar_ && lparam) {
                    auto* r=reinterpret_cast<RECT*>(lparam); RECT current{}; ::GetWindowRect(hwnd,&current);
                    const LONG height=r->bottom-r->top; r->top=current.top; r->bottom=r->top+height;
                    *r=platform::ClampRectToBounds(*r,monitor_.work_area); return TRUE;
                } break;

            case WM_COMMAND:
                if (hwnd == mode_toolbar_ && HIWORD(wparam) == BN_CLICKED &&
                    LOWORD(wparam) >= kFirstModeCommand &&
                    LOWORD(wparam) < kFirstModeCommand + static_cast<int>(kModes.size())) {
                    OnPaletteClicked(LOWORD(wparam) - kFirstModeCommand);
                }
                return 0;
            case WM_KEYDOWN:
                if (hwnd == mode_toolbar_) { return HandleKey(wparam); }
                break;
            case WM_PRINTCLIENT:
                if (hwnd == mode_toolbar_) { DrawPalette(reinterpret_cast<HDC>(wparam)); }
                else if (hwnd == magnifier_) { RECT client{}; ::GetClientRect(hwnd, &client); PaintMagnifier(reinterpret_cast<HDC>(wparam), client); }
                return 0;
            case theme::kChangedMessage:
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            case WM_ERASEBKGND:
                return 1;

            case WM_NCHITTEST:
                if (hwnd == mode_toolbar_) {
                    POINT point{GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)}; ::ScreenToClient(hwnd,&point);
                    return ::ChildWindowFromPointEx(hwnd,point,CWP_SKIPINVISIBLE)==hwnd ? HTCAPTION:HTCLIENT;
                }
                return HTTRANSPARENT;  // Image and chrome remain click-through.

            case WM_PAINT: {
                if (hwnd != image_) {
                    buffer_.Paint(hwnd, [this, hwnd](HDC dc, const RECT& client) {
                        if (hwnd == magnifier_) { PaintMagnifier(dc, client); }
                        else if (hwnd == mode_toolbar_) {
                            theme::FillSolid(dc, client, theme::kChromeColorKey);
                            DrawPalette(dc);
                        }
                        else if(hwnd==shade_ || IsShadeStrip(hwnd)) { theme::FillSolid(dc,client,theme::kDim); }
                        else { PaintChrome(dc); }
                    });
                    return 0;
                }
                PAINTSTRUCT ps{};
                HDC dc = ::BeginPaint(hwnd, &ps);
                if (dc != nullptr) {
                    RECT bounds{};
                    ::GetWindowRect(hwnd, &bounds);
                    controller_.PaintImage(dc, bounds, ps.rcPaint);
                }
                ::EndPaint(hwnd, &ps);
                return 0;
            }

            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }

    LRESULT HandleKey(WPARAM key) {
        const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const int step = shift ? 10 : 1;
        switch (key) {
            case VK_ESCAPE:
                controller_.HandleCancelled();
                return 0;
            case VK_RETURN:
                if (controller_.workspace().scrollshot_visible()) {
                    controller_.RequestScrollshotCommand(ScrollshotCommand::Start);
                } else if (controller_.selector().CanCommit()) {
                    controller_.EnterEditing();
                }
                return 0;
            case 'A':
                // Ctrl + A selects the monitor the overlay window covers.
                if (controller_.workspace().can_adjust_selection() && (::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                    if (controller_.workspace().editing()) { controller_.selector().SetSelection(monitor_.bounds); }
                    else { controller_.selector().SetImplicitSelection(monitor_.bounds); }
                    controller_.RegionChanged();
                }
                return 0;
            case 'C':
                if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                    if (controller_.workspace().magnifier_visible()) { CopyDisplayedColor(); }
                }
                return 0;
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN: {
                if (!controller_.workspace().can_adjust_selection()) { return 0; }
                const bool resize = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
                const int dx = (key == VK_LEFT) ? -step : (key == VK_RIGHT ? step : 0);
                const int dy = (key == VK_UP) ? -step : (key == VK_DOWN ? step : 0);
                controller_.selector().OnNudge(dx, dy, resize);
                controller_.RegionChanged();
                return 0;
            }
            default:
                return 0;
        }
    }

    // -----------------------------------------------------------------------
    // Palette
    // -----------------------------------------------------------------------

    void OnPaletteClicked(int index) {
        if (!controller_.workspace().can_switch_mode()) { return; }
        if (index < 0 || index >= static_cast<int>(kModes.size())) {
            return;
        }
        const app::CaptureMode mode = kModes[static_cast<size_t>(index)];
        if (mode == app::CaptureMode::Scrollshot && !CaptureExclusionSupported()) { return; }
        if (!controller_.AllowMode(mode)) { return; }
        controller_.SetMode(mode);
        if (controller_.mode() != app::CaptureMode::Record) { ::SetFocus(dim_); }
    }

    /// One width for all mode buttons: the widest label in the active
    /// language plus the icon and the button's own insets.
    void MeasureModeButtons() {
        const float scale = monitor_.scale();
        LONG widest = 0;
        if (HDC dc = ::CreateCompatibleDC(nullptr)) {
            const HGDIOBJ previous = ::SelectObject(dc, button_font_);
            for (UINT id : {IDS_MODE_SCREENSHOT, IDS_MODE_SCROLLSHOT, IDS_MODE_RECORD}) {
                const wchar_t* text = strings::Get(id);
                SIZE size{};
                if (::GetTextExtentPoint32W(dc, text, ::lstrlenW(text), &size)) { widest = std::max(widest, size.cx); }
            }
            ::SelectObject(dc, previous);
            ::DeleteDC(dc);
        }
        // Face inset 6, icon inset 10, icon 18, gap 6, label inset 6 each side.
        mode_button_width_ = std::max<LONG>(theme::Scaled(theme::kPaletteButtonMinimumWidth, scale),
                                            widest + theme::Scaled(60, scale));
    }

    /// Palette buttons in this window's client coordinates.
    [[nodiscard]] std::array<PaletteButton, 3> PaletteLayout(bool local = false) const {
        const float scale = monitor_.scale();
        const LONG button_width = mode_button_width_;
        const LONG button_height = theme::Scaled(theme::kPaletteButtonHeight, scale);
        const LONG padding = theme::Scaled(theme::kPalettePadding, scale);
        const LONG grip=theme::Scaled(24,scale);
        const LONG count = static_cast<LONG>(kModes.size());
        const LONG panel_width = button_width * count + padding * (count + 1) + grip;
        const LONG panel_left = local ? 0 : (client_width() - panel_width) / 2;
        const LONG panel_top = 0;

        constexpr std::array<UINT, 3> labels{IDS_MODE_SCREENSHOT, IDS_MODE_SCROLLSHOT, IDS_MODE_RECORD};
        std::array<PaletteButton, 3> buttons{};
        LONG left = panel_left + padding + grip;
        for (size_t i = 0; i < buttons.size(); ++i) {
            buttons[i].mode = kModes[i];
            buttons[i].label = strings::Get(labels[i]);
            buttons[i].rect = RECT{left, panel_top + padding, left + button_width, panel_top + padding + button_height};
            left += button_width + padding;
        }
        return buttons;
    }

    [[nodiscard]] int PaletteHitTest(POINT virtual_pt) const {
        if (!owns_palette_ || !controller_.workspace().can_switch_mode()) {
            return -1;
        }
        POINT pt=virtual_pt; ::ScreenToClient(mode_toolbar_,&pt);
        const auto buttons = PaletteLayout(true);
        for (size_t i = 0; i < buttons.size(); ++i) {
            if (::PtInRect(&buttons[i].rect, pt) != FALSE) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    [[nodiscard]] RECT ToolbarPanel() const {
        const auto buttons = PaletteLayout();
        const LONG padding = theme::Scaled(theme::kPalettePadding, monitor_.scale());
        return RECT{buttons.front().rect.left - padding-theme::Scaled(24,monitor_.scale()), buttons.front().rect.top - padding,
                    buttons.back().rect.right + padding, buttons.back().rect.bottom + padding};
    }

    [[nodiscard]] bool ToolbarContains(POINT virtual_pt) const {
        RECT panel{}; if(mode_toolbar_) { ::GetWindowRect(mode_toolbar_,&panel); }
        return owns_palette_ && ::IsWindowVisible(mode_toolbar_) && ::PtInRect(&panel,virtual_pt);
    }

    // -----------------------------------------------------------------------
    // Painting
    // -----------------------------------------------------------------------

    void CopyDisplayedColor() {
        const auto color = controller_.SamplePixel(controller_.pointer());
        bool copied = false;
        if (color) {
            const std::wstring value = FormatCaptureColor(
                *color, controller_.color_readout().representation());
            const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory != nullptr) {
                void* text = ::GlobalLock(memory);
                if (text != nullptr) {
                    std::memcpy(text, value.c_str(), bytes);
                    ::GlobalUnlock(memory);
                    if (::OpenClipboard(dim_)) {
                        if (::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT, memory)) {
                            copied = true;
                            memory = nullptr;  // Clipboard now owns the allocation.
                        }
                        ::CloseClipboard();
                    }
                }
                if (memory != nullptr) {
                    ::GlobalFree(memory);
                }
            }
        }
        controller_.SetNotice(strings::Get(copied ? IDS_SHELL_COLOR_COPIED :
            (color ? IDS_SHELL_COLOR_COPY_FAILED : IDS_SHELL_COLOR_UNAVAILABLE)));
        ::SetTimer(dim_, kNoticeTimerId, kNoticeTimeoutMs, nullptr);
    }

    void PaintMagnifier(HDC dc, const RECT& client) const {
        const float scale = monitor_.scale();
        const int cell = theme::Scaled(theme::kMagnifierCell, scale);
        const int count = theme::kMagnifierGridPixels;
        const int padding = theme::Scaled(theme::kMagnifierPadding, scale);
        const int grid = cell * count;
        const int left = (client.right - grid) / 2;
        const int line = theme::Scaled(theme::kMagnifierLineHeight, scale);
        const POINT cursor = controller_.pointer();
        theme::FillSolid(dc, client, theme::Current().panel);

        const HBRUSH brush = static_cast<HBRUSH>(::GetStockObject(DC_BRUSH));
        for (int y = 0; y < count; ++y) {
            for (int x = 0; x < count; ++x) {
                const auto color = controller_.SamplePixel(
                    POINT{cursor.x + x - count / 2, cursor.y + y - count / 2});
                const RECT pixel{left + x * cell, padding + y * cell,
                                 left + (x + 1) * cell, padding + (y + 1) * cell};
                ::SetDCBrushColor(dc, color.value_or(theme::Current().border));
                ::FillRect(dc, &pixel, brush);
            }
        }
        // The two-tone reticle sits outside the sampled pixel. At the smaller
        // four-times zoom, no grid or crosshair obscures the pixel's real color.
        RECT center{left + (count / 2) * cell, padding + (count / 2) * cell,
                    left + (count / 2 + 1) * cell, padding + (count / 2 + 1) * cell};
        ::InflateRect(&center, 2, 2);
        theme::FrameSolid(dc, center, RGB(255,255,255), 1);
        ::InflateRect(&center, -1, -1);
        theme::FrameSolid(dc, center, theme::kDim, 1);

        const std::wstring coordinates = strings::Format(
            IDS_SHELL_MAGNIFIER_COORDINATES, {std::to_wstring(cursor.x), std::to_wstring(cursor.y)});
        const auto color = controller_.SamplePixel(cursor);
        const std::wstring value = color ? FormatCaptureColor(
            *color, controller_.color_readout().representation()) : strings::Get(IDS_SHELL_MAGNIFIER_NO_PIXEL);
        const std::array<const wchar_t*, 3> labels{
            value.c_str(), coordinates.c_str(), strings::Get(IDS_SHELL_MAGNIFIER_HINT)};
        for (size_t i = 0; i < labels.size(); ++i) {
            const LONG top = padding * 2 + grid + static_cast<LONG>(i) * line;
            const RECT row{padding, top, client.right - padding, top + line};
            theme::DrawLabel(dc, row, labels[i], i < 2 ? theme::Current().text : theme::Current().muted_text,
                label_font_, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
    }

    void PaintDim(HDC dc) const {
        theme::FillSolid(dc,{0,0,client_width(),client_height()},theme::kDim);
    }

    /// Fully opaque chrome. Everything left in the transparency key colour
    /// disappears, so only the panels and outlines are visible.
    void PaintChrome(HDC dc) const {
        const RECT client{0, 0, client_width(), client_height()};
        theme::FillSolid(dc, client, theme::kChromeColorKey);

        controller_.selector().RenderGdi(dc, monitor_.bounds, monitor_.scale(), label_font_,
            controller_.workspace().editing() && controller_.workspace().can_adjust_selection(), controller_.workspace().accent(),
            controller_.workspace().scrollshot_locked(), !controller_.document_view());
        if (controller_.document_view()) { DrawDocumentIndicators(dc); }
        if (owns_palette_) { DrawNotice(dc); }
    }

    /// Readout of a document view: image size and scale, at the place the
    /// frame's size readout normally takes.
    [[nodiscard]] RECT DocumentLabelRect() const {
        const std::wstring& label = controller_.document_label();
        LONG width = 0;
        if (HDC dc = ::CreateCompatibleDC(nullptr)) {
            const HGDIOBJ previous = ::SelectObject(dc, label_font_);
            SIZE size{};
            if (::GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &size)) { width = size.cx; }
            ::SelectObject(dc, previous);
            ::DeleteDC(dc);
        }
        return RegionSelector::LabelRect(controller_.selector().selection(), monitor_.bounds, monitor_.scale(),
                                         width + theme::Scaled(20, monitor_.scale()));
    }

    /// The strip at the viewport's right edge that shows where it is in the image.
    [[nodiscard]] RECT DocumentScrollRect() const {
        const RECT selection = controller_.selector().selection();
        const float scale = monitor_.scale();
        const LONG inset = theme::Scaled(theme::kSelectionBorder, scale) + theme::Scaled(4, scale);
        return RECT{selection.right - monitor_.bounds.left - inset - theme::Scaled(5, scale),
                    selection.top - monitor_.bounds.top + inset,
                    selection.right - monitor_.bounds.left - inset,
                    selection.bottom - monitor_.bounds.top - inset};
    }

    void DrawDocumentIndicators(HDC dc) const {
        const float scale = monitor_.scale();
        if (!controller_.document_label().empty()) {
            const RECT box = DocumentLabelRect();
            theme::FillRounded(dc, box, theme::Scaled(4.0f, scale), theme::Current().panel);
            theme::DrawLabel(dc, box, controller_.document_label().c_str(), theme::Current().text, label_font_,
                             DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
        const float visible = controller_.document_visible_fraction();
        if (visible >= 1.0f) { return; }  // The whole image fits: nothing to indicate.
        const RECT track = DocumentScrollRect();
        const LONG length = platform::RectHeight(track);
        if (length <= 0) { return; }
        const LONG shortest = std::min<LONG>(length, theme::Scaled(24, scale));
        const LONG thumb = std::clamp<LONG>(static_cast<LONG>(visible * static_cast<float>(length)), shortest, length);
        const LONG top = track.top + static_cast<LONG>(std::min(controller_.document_top_fraction() / (1.0f - visible), 1.0f) *
                                                       static_cast<float>(length - thumb));
        theme::FillRounded(dc, RECT{track.left, top, track.right, top + thumb}, theme::Scaled(2.5f, scale),
                           theme::Mix(theme::Current().panel, theme::Current().text, 0.55f));
    }

public:
    /// Repaints what a scroll or zoom step changes besides the image itself.
    void InvalidateDocumentIndicators() {
        if (chrome_ == nullptr) { return; }
        RECT label = DocumentLabelRect();
        ::InflateRect(&label, 1, 1);
        RECT track = DocumentScrollRect();
        ::InflateRect(&track, 1, 1);
        for (const RECT* rect : {&label, &track, &document_label_painted_}) {
            if (!::IsRectEmpty(rect)) { ::InvalidateRect(chrome_, rect, FALSE); }
        }
        document_label_painted_ = label;
    }

private:

    void DrawPalette(HDC dc) const {
        const auto buttons = PaletteLayout(true);
        const LONG padding = theme::Scaled(theme::kPalettePadding, monitor_.scale());
        const RECT panel{0, 0, buttons.back().rect.right + padding, buttons.back().rect.bottom + padding};
        theme::DrawPanel(dc, panel, monitor_.scale());
        theme::DrawGrip(dc,{0,0,theme::Scaled(24,monitor_.scale()),panel.bottom},monitor_.scale());
    }

    void DrawNotice(HDC dc) const {
        const float scale = monitor_.scale();
        const LONG padding = theme::Scaled(theme::kPalettePadding, scale);
        const RECT panel = ToolbarPanel();
        // Clipboard notices remain independent of the mode toolbar's opacity.
        const std::wstring& notice = controller_.notice();
        if (notice.empty()) {
            return;
        }
        const LONG width = theme::Scaled(360.0f, scale);
        const LONG height = theme::Scaled(34.0f, scale);
        const LONG left = (panel.left + panel.right) / 2 - width / 2;
        const LONG top = panel.bottom + padding;
        const RECT box{left, top, left + width, top + height};
        theme::FillRounded(dc, box, theme::Scaled(6.0f, scale), theme::Current().panel);
        theme::DrawLabel(dc, box, notice.c_str(), theme::Current().text, label_font_,
                         DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }

    OverlayController& controller_;
    platform::MonitorInfo monitor_;
    bool owns_palette_{false};
    HWND dim_{nullptr};
    HWND shade_{nullptr};
    /// Replaces `shade_` while a Scrollshot frame is exposed: top, bottom, left, right.
    std::array<HWND, 4> shade_strips_{};
    HWND chrome_{nullptr};
    HWND mode_toolbar_{nullptr};
    std::array<HWND, 3> mode_buttons_{};
    LONG mode_button_width_{0};
    std::optional<ULONGLONG> mode_fade_start_;
    RECT mode_home_{};
    bool mode_suppressed_{false};
    HWND magnifier_{nullptr};
    HWND image_{nullptr};
    FrozenBitmap frozen_;
    BackBuffer buffer_;
    HFONT label_font_{nullptr};
    HFONT button_font_{nullptr};
    int hover_button_{-1};
    RECT document_label_painted_{};
};

// ---------------------------------------------------------------------------
// OverlayController
// ---------------------------------------------------------------------------

OverlayController::OverlayController(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

OverlayController::~OverlayController() { Destroy(); }

bool OverlayController::Show(const capture::DesktopSnapshot& snapshot, POINT initiating_pointer) {
    Destroy();

    selector_.Reset(snapshot.virtual_bounds.right > snapshot.virtual_bounds.left ? snapshot.virtual_bounds
                                                                                 : platform::VirtualScreenBounds());
    workspace_.Reset();
    pointer_sequence_.Cancel();
    pointer_active_ = false;
    selection_started_ = false;
    color_readout_ = {};
    notice_.clear();

    pointer_ = initiating_pointer;
    const platform::MonitorInfo palette_monitor = platform::MonitorForPoint(initiating_pointer);
    selector_.set_enabled(true);
    selector_.SetImplicitSelection(palette_monitor.bounds);
    last_region_ = selector_.selection();
    last_region_interacting_ = false;
    last_has_selection_ = selector_.has_selection();
    selector_.OnMouseMove(initiating_pointer, false);

    for (const platform::MonitorInfo& monitor : platform::EnumerateMonitors()) {
        const capture::MonitorFrame* frame = nullptr;
        for (const capture::MonitorFrame& candidate : snapshot.monitors) {
            if (::EqualRect(&candidate.desktop_rect, &monitor.bounds)) {
                frame = &candidate;
                break;
            }
        }
        auto window = std::make_unique<OverlayWindow>(*this, monitor, monitor.handle == palette_monitor.handle);
        if (!window->Create(frame)) {
            platform::LogMessage(L"Failed to create an overlay window; aborting overlay");
            Destroy();
            return false;
        }
        windows_.push_back(std::move(window));
    }

    if (windows_.empty()) {
        return false;
    }
    for (const auto& window : windows_) {
        if (window->owns_palette()) {
            window->Activate();
        }
    }
    visible_ = true;
    UpdatePointer(initiating_pointer);
    return true;
}

void OverlayController::Hide() {
    color_readout_.CancelPress();
    for (const auto& window : windows_) {
        window->Hide();
    }
    visible_ = false;
}

void OverlayController::Resume() {
    visible_ = true;
    for (const auto& window : windows_) { window->Resume(); }
    UpdatePointer(pointer_);
}

void OverlayController::FixScreenshot() {
    if (!workspace_.screenshot_visible()) { return; }
    workspace_.FixScreenshot();
    selector_.set_enabled(false);
    for (const auto& window : windows_) { window->BeginModeFade(); }
    Invalidate();
}

void OverlayController::BeginScrollshotCapture() {
    if (!workspace_.editing() || workspace_.screenshot_fixed() || workspace_.scrollshot_locked()) { return; }
    CancelPointer();
    workspace_.BeginScrollshotCapture();
    selector_.set_enabled(false);
    // Mode switching ends with the lock; the options fade as they do when a
    // screenshot is fixed.
    for (const auto& window : windows_) { window->BeginModeFade(); }
    Invalidate();
}

void OverlayController::StopScrollshotCapture() {
    if (!workspace_.scrollshot_capturing()) { return; }
    workspace_.StopScrollshotCapture();
    ExposeScrollshotFrame(false);
    Invalidate();
}

void OverlayController::ExposeScrollshotFrame(bool exposed) {
    if (frame_exposed_ == exposed || windows_.empty()) { return; }
    frame_exposed_ = exposed;
    for (const auto& window : windows_) { window->SetFrameExposed(exposed); }
    // The picture taken next must already be composed with the new shading.
    if (exposed) { ::DwmFlush(); }
}

void OverlayController::RequestScrollshotCommand(ScrollshotCommand command) {
    if (callbacks_.on_scrollshot_command) { callbacks_.on_scrollshot_command(command); }
}

void OverlayController::BeginDocumentView(const RECT& viewport) {
    if (windows_.empty() || document_view_) { return; }
    CancelPointer();
    if (workspace_.scrollshot_locked()) {
        workspace_.StopScrollshotCapture();
        workspace_.EditScrollshot();
    } else {
        // A workspace shown only for this document: no selection was made.
        workspace_.EnterEditing();
        workspace_.EditorFeatureUsed();
    }
    document_view_ = true;
    document_top_ = 0.0f;
    document_visible_ = 1.0f;
    document_label_.clear();
    selector_.SetSelection(viewport);
    selector_.set_enabled(false);
    last_region_ = selector_.selection();
    last_has_selection_ = selector_.has_selection();
    last_region_interacting_ = false;
    for (const auto& window : windows_) { window->BeginModeFade(); }
    Invalidate();
}

void OverlayController::SetDocumentView(const RECT& viewport, float top_fraction, float visible_fraction,
                                        std::wstring label) {
    if (!document_view_) { return; }
    document_top_ = top_fraction;
    document_visible_ = visible_fraction;
    document_label_ = std::move(label);
    const RECT previous = selector_.selection();
    if (::EqualRect(&previous, &viewport) == FALSE) {
        selector_.SetSelection(viewport);
        last_region_ = selector_.selection();
        last_has_selection_ = selector_.has_selection();
        Invalidate();
    }
    for (const auto& window : windows_) { window->InvalidateDocumentIndicators(); }
}

void OverlayController::EditorWheel(int delta, bool control, bool shift, POINT point) {
    if (callbacks_.on_editor_wheel) { callbacks_.on_editor_wheel(delta, control, shift, point); }
}

void OverlayController::Destroy() {
    windows_.clear();
    visible_ = false;
    frame_pressed_ = false;
    document_view_ = false;
    frame_exposed_ = false;
    document_label_.clear();
    notice_.clear();
}

bool OverlayController::AllowMode(app::CaptureMode mode) {
    if (mode != app::CaptureMode::Record || recording_available_) { return true; }
    if (callbacks_.on_recording_unavailable) { callbacks_.on_recording_unavailable(); }
    return false;
}

void OverlayController::SetMode(app::CaptureMode mode) {
    if (!workspace_.can_switch_mode() || mode == workspace_.mode()) { return; }
    workspace_.SetMode(mode);
    if (workspace_.editing() && callbacks_.on_editing_changed) {
        callbacks_.on_editing_changed(mode, selector_.selection());
    }
    Invalidate();
}

RECT OverlayController::mode_island_rect() const {
    if (!visible_ || !workspace_.can_switch_mode()) { return {}; }
    for (const auto& window : windows_) { if (window->owns_palette()) { return window->ModeToolbarRect(); } }
    return {};
}

void OverlayController::ModeIslandMoved() {
    if (callbacks_.on_mode_island_moved) { callbacks_.on_mode_island_moved(); }
}

void OverlayController::UpdateModeIsland() {
    const bool suppressed = frame_pressed_ || selector_.interacting();
    for (const auto& window : windows_) { window->SetModeToolbarSuppressed(suppressed); }
}

HWND OverlayController::owner_window() const noexcept {
    for (const auto& window : windows_) { if (window->owns_palette()) { return window->hwnd(); } }
    return nullptr;
}

HWND OverlayController::panel_owner_window() const noexcept {
    for (const auto& window : windows_) { if (window->owns_palette()) { return window->panel_owner(); } }
    return nullptr;
}

void OverlayController::EnterEditing() {
    if (workspace_.editing() || !selector_.CanCommit()) { return; }
    workspace_.EnterEditing();
    selector_.SetSelection(selector_.selection());
    last_region_ = selector_.selection();
    last_region_interacting_ = selector_.interacting();
    last_has_selection_ = selector_.has_selection();
    UpdatePointer(pointer_);
    // The mode island takes its place first: the island that opens now stays clear of it.
    UpdateModeIsland();
    if (callbacks_.on_editing_changed) { callbacks_.on_editing_changed(mode(), selector_.selection()); }
    Invalidate();
}

void OverlayController::RegionChanged() {
    const RECT region = selector_.selection();
    const bool geometry_changed = !::EqualRect(&region, &last_region_) ||
        last_has_selection_ != selector_.has_selection();
    if (!geometry_changed && last_region_interacting_ == selector_.interacting()) { return; }
    const RECT previous = last_region_;
    const bool previous_has_selection = last_has_selection_;
    last_region_ = region;
    last_has_selection_ = selector_.has_selection();
    last_region_interacting_ = selector_.interacting();
    // Before the islands hear of the new frame: they place themselves around
    // the mode island.
    UpdateModeIsland();
    if (workspace_.editing() && callbacks_.on_region_changed) {
        callbacks_.on_region_changed(region);
    }
    if (geometry_changed) {
        for (const auto& window : windows_) { window->InvalidateSelection(previous, previous_has_selection); }
    }
}

bool OverlayController::UsesAnnotationTool() const {
    return workspace_.screenshot_visible() && callbacks_.uses_annotation_tool && callbacks_.uses_annotation_tool();
}

HCURSOR OverlayController::AnnotationCursor(POINT point) const {
    const auto r=selector_.selection();
    if(!workspace_.screenshot_visible() || !::PtInRect(&r,point) || !callbacks_.annotation_cursor) { return nullptr; }
    if(!workspace_.screenshot_fixed()) {
        const auto handle=selector_.HitTest(point, platform::MonitorForPoint(point).scale());
        if(handle!=SelectionHandle::Body && handle!=SelectionHandle::None) { return nullptr; }
    }
    return callbacks_.annotation_cursor(point);
}

void OverlayController::PointerDown(POINT point, bool native_double, bool excluded, DWORD time) {
    // A locked Scrollshot frame neither moves nor starts a new selection.
    if (workspace_.scrollshot_locked()) { return; }
    const bool annotation=callbacks_.wants_annotation_pointer ? callbacks_.wants_annotation_pointer(point):UsesAnnotationTool();
    WorkspacePointerTarget target = WorkspacePointerTarget::Content;
    if (excluded) { target = WorkspacePointerTarget::Excluded; }
    else if (workspace_.editing()) {
        const auto handle = selector_.HitTest(point, platform::MonitorForPoint(point).scale());
        if (workspace_.screenshot_fixed()) {
            const RECT region = selector_.selection();
            target = ::PtInRect(&region, point) ?
                (annotation ? WorkspacePointerTarget::Annotation : WorkspacePointerTarget::Content) :
                WorkspacePointerTarget::Creation;
        }
        else if (handle != SelectionHandle::None && handle != SelectionHandle::Body) { target = WorkspacePointerTarget::Handle; }
        else if (handle == SelectionHandle::None) { target = WorkspacePointerTarget::Creation; }
        else if (annotation) { target = WorkspacePointerTarget::Annotation; }
    }
    if (target == WorkspacePointerTarget::Handle) {
        frame_pressed_ = true;
        UpdateModeIsland();
        if (callbacks_.on_frame_interacting) { callbacks_.on_frame_interacting(true); }
    }
    pointer_sequence_.Down(point, time, native_double, target, WorkspacePointerMetrics::Native());
    pointer_active_ = true;
    selection_started_ = false;
    if (target == WorkspacePointerTarget::Annotation && callbacks_.on_image_pointer) {
        callbacks_.on_image_pointer(native_double && callbacks_.wants_annotation_pointer ? WM_LBUTTONDBLCLK:WM_LBUTTONDOWN, point);
    }
}

void OverlayController::PointerMove(POINT point, bool button_down) {
    if (!pointer_active_ || !button_down) { selector_.OnMouseMove(point, false); return; }
    pointer_sequence_.Move(point);
    const auto target = pointer_sequence_.target();
    if (target == WorkspacePointerTarget::Excluded) { return; }
    if (target == WorkspacePointerTarget::Annotation) {
        if (callbacks_.on_image_pointer) { callbacks_.on_image_pointer(WM_MOUSEMOVE, point); }
        return;
    }
    if (workspace_.can_adjust_selection() && pointer_sequence_.dragged()) {
        if (!selection_started_) {
            selector_.OnMouseDown(pointer_sequence_.origin());
            selection_started_ = true;
        }
        selector_.OnMouseMove(point, true);
        RegionChanged();
    }
}

void OverlayController::PointerUp(POINT point) {
    if (!pointer_active_) { return; }
    PointerMove(point, true);
    const auto target = pointer_sequence_.target();
    const bool quick_copy = pointer_sequence_.Up(point);
    pointer_active_ = false;
    frame_pressed_ = false;
    if (target == WorkspacePointerTarget::Excluded) { return; }
    if (target == WorkspacePointerTarget::Annotation) {
        if (quick_copy && !callbacks_.wants_annotation_pointer) {
            // The second release completes the screenshot, not a second mark.
            // A drag clears quick_copy and follows the ordinary drawing path.
            if (callbacks_.on_image_pointer) { callbacks_.on_image_pointer(WM_CAPTURECHANGED, point); }
            CompleteScreenshot();
            return;
        }
        if (callbacks_.on_image_pointer) { callbacks_.on_image_pointer(WM_LBUTTONUP, point); }
        if (visible_ && !callbacks_.wants_annotation_pointer && !pointer_sequence_.dragged() && workspace_.ConsumeQuickReturn()) { CompleteScreenshot(); }
        return;
    }
    if (selection_started_) { selector_.OnMouseUp(point); RegionChanged(); }
    UpdateModeIsland();
    if(callbacks_.on_frame_interacting) { callbacks_.on_frame_interacting(false); }
    selection_started_ = false;
    if (!workspace_.editing()) { EnterEditing(); }
    const bool returned_click = !pointer_sequence_.dragged() &&
        (target == WorkspacePointerTarget::Content || target == WorkspacePointerTarget::Creation) &&
        workspace_.ConsumeQuickReturn();
    if (returned_click || (quick_copy && target == WorkspacePointerTarget::Content && mode() == app::CaptureMode::Screenshot)) {
        CompleteScreenshot();
    }
}

void OverlayController::CancelPointer() {
    if (!pointer_active_) { return; }
    if (pointer_sequence_.target() == WorkspacePointerTarget::Annotation && callbacks_.on_image_pointer) {
        callbacks_.on_image_pointer(WM_CAPTURECHANGED, pointer_);
    }
    frame_pressed_ = false;
    if (selection_started_) {
        selector_.OnMouseUp(selector_.cursor());
        RegionChanged();
    }
    UpdateModeIsland();
    if(callbacks_.on_frame_interacting) { callbacks_.on_frame_interacting(false); }
    pointer_active_ = selection_started_ = false;
    pointer_sequence_.Cancel();
}

void OverlayController::EditorKey(UINT message, WPARAM key, LPARAM flags) {
    if (callbacks_.on_editor_key) { callbacks_.on_editor_key(message, key, flags); }
}

void OverlayController::PaintImage(HDC dc, const RECT& bounds, const RECT& repaint) const {
    if (callbacks_.on_paint_image) { callbacks_.on_paint_image(dc, bounds, repaint); }
}

bool OverlayController::MagnifierContains(POINT point) const {
    for (const auto& window : windows_) { if (window->MagnifierContains(point)) { return true; } }
    return false;
}

void OverlayController::UpdatePointer(POINT cursor) {
    pointer_ = cursor;
    for (const auto& window : windows_) {
        window->UpdateMagnifier();
    }
}

std::optional<COLORREF> OverlayController::SamplePixel(POINT cursor) const {
    for (const auto& window : windows_) {
        if (const auto color = window->SamplePixel(cursor)) {
            return color;
        }
    }
    return std::nullopt;
}

void OverlayController::Invalidate() {
    for (const auto& window : windows_) {
        window->Invalidate();
    }
}

void OverlayController::InvalidateImage(const RECT& desktop_damage) {
    for (const auto& window : windows_) { window->InvalidateImage(desktop_damage); }
}

void OverlayController::SetNotice(std::wstring text) {
    notice_ = std::move(text);
    Invalidate();
}

void OverlayController::CompleteScreenshot() {
    // Fixed screenshots disable selector manipulation, not image completion.
    if (mode() == app::CaptureMode::Screenshot &&
        (workspace_.screenshot_fixed() || selector_.CanCommit()) && callbacks_.on_screenshot_complete) {
        callbacks_.on_screenshot_complete();
    }
}

void OverlayController::HandleCancelled() {
    if (callbacks_.on_cancelled) {
        callbacks_.on_cancelled();
    }
}

}  // namespace easnap::ui

namespace easnap::ui {
bool OverlayController::ProcessDialogMessage(MSG& message) const {
    if (!visible_) { return false; }
    for (const auto& window : windows_) { if (window->ProcessDialogMessage(message)) { return true; } }
    return false;
}
}
