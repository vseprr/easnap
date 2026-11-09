// Easnap - screenshots pinned to the screen (ADR-038).
#include "ui/pinned_images.h"

#include "platform/image_export.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/native_controls.h"
#include "ui/paint_buffer.h"
#include "ui/pin_layout.h"
#include "ui/ui_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <new>
#include <optional>
#include <utility>

namespace easnap::ui {
namespace {

constexpr wchar_t kPinClassName[] = L"EasnapPinnedImage";

float WindowScale(HWND hwnd) noexcept {
    const UINT dpi = ::GetDpiForWindow(hwnd);
    return static_cast<float>(dpi != 0 ? dpi : 96) / 96.0f;
}
RECT WindowRect(HWND hwnd) noexcept {
    RECT r{};
    ::GetWindowRect(hwnd, &r);
    return r;
}
POINT Centre(const RECT& r) noexcept { return {r.left + (r.right - r.left) / 2, r.top + (r.bottom - r.top) / 2}; }
bool SameZoom(double a, double b) noexcept { return std::fabs(a - b) < 1e-9; }

} // namespace

class PinnedImages::Window final {
public:
    Window(PinnedImages& owner, platform::BgraImage image)
        : owner_(owner), image_(std::make_shared<const platform::BgraImage>(std::move(image))) {}
    ~Window() {
        // A copy or save still running is dropped before its window goes.
        export_job_.Discard();
        CloseCaptureNotice(export_notice_);
        if (hwnd_ != nullptr) { ::DestroyWindow(hwnd_); }
    }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    [[nodiscard]] bool Create(const RECT& source);
    /// Closes the pin now, or when its menu or Save dialog returns.
    void RequestClose();
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    /// Windows is ending the session: a save is waited for, a copy cancelled.
    void SettleExportForSessionEnd() noexcept { export_job_.SettleForSessionEnd(); }

private:
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam);
    [[nodiscard]] SIZE image_size() const noexcept { return image_ ? SIZE{image_->width, image_->height} : SIZE{}; }
    void Paint();
    /// The whole client area, border and image, into `dc`.
    void PaintTo(HDC dc);
    /// Only the border shows whether the pin is active; the image stays.
    void InvalidateBorder();
    void Zoom(double zoom, POINT anchor);
    void Wheel(int delta, POINT anchor);
    void ShowMenu(POINT at);
    void Copy();
    void Save();
    /// Copies or saves on a worker; the pin is busy until it has finished.
    bool StartExport(platform::ImageExportTarget target, ImageSaveDestination destination);
    /// Handles kMsgImageExportCompleted.
    void OnExportCompleted(platform::ImageExportJob::Generation generation);
    void KeepOnScreen();
    /// Ends a menu or dialog. A close asked for meanwhile happens now; true
    /// when the window is gone.
    bool EndBusy();

    PinnedImages& owner_;
    /// Shared with a copy or save that is running.
    std::shared_ptr<const platform::BgraImage> image_;
    HWND hwnd_{nullptr};
    platform::ImageExportJob export_job_;
    ImageSaveDestination export_destination_;
    /// "Copying..." or "Saving..." while the export lasts.
    HWND export_notice_{nullptr};
    double zoom_{1.0};
    int wheel_delta_{0};
    int busy_{0};
    bool close_requested_{false};
    bool active_{false};
};

bool PinnedImages::Window::Create(const RECT& source) {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        // No CS_DROPSHADOW: Windows draws that shadow as a window of its own
        // in this process, unmarked and not excluded, which would make every
        // capture hide the workspace and refuse Scrollshot (ADR-038).
        wc.lpfnWndProc = Procedure;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kPinClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!registered || !image_ || image_->empty()) { return false; }
    const platform::MonitorInfo monitor = platform::MonitorForRect(source);
    const PinPlacement placement = InitialPinPlacement(image_size(), source, monitor.work_area, monitor.scale());
    zoom_ = placement.zoom;
    const RECT& r = placement.window;
    // Not layered and without caption styles: a pin needs neither, and it is
    // captured like any other window (ADR-038).
    if (::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPinClassName, strings::Get(IDS_PIN_WINDOW_NAME), WS_POPUP,
                          r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                          ::GetModuleHandleW(nullptr), this) == nullptr) {
        platform::LogMessage(L"CreateWindowEx(pinned image) failed (error %lu)", ::GetLastError());
        return false;
    }
    MarkAsScreenContent(hwnd_);
    ::ShowWindow(hwnd_, SW_SHOW);
    ::SetForegroundWindow(hwnd_);
    return true;
}

void PinnedImages::Window::RequestClose() {
    if (hwnd_ == nullptr) { return; }
    if (busy_ != 0) {
        close_requested_ = true;
        return;
    }
    ::DestroyWindow(hwnd_);
}

bool PinnedImages::Window::EndBusy() {
    --busy_;
    if (busy_ == 0 && close_requested_ && hwnd_ != nullptr) {
        close_requested_ = false;
        ::DestroyWindow(hwnd_);
    }
    return hwnd_ == nullptr;
}

LRESULT CALLBACK PinnedImages::Window::Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* created = static_cast<Window*>(reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
        created->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
    }
    auto* self = reinterpret_cast<Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self == nullptr) { return ::DefWindowProcW(hwnd, message, wparam, lparam); }
    if (message == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        RemoveScreenContentMark(hwnd);
        self->hwnd_ = nullptr;
        self->image_.reset(); // The image goes with its window.
        return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    return self->Handle(message, wparam, lparam);
}

LRESULT PinnedImages::Window::Handle(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_NCHITTEST: {
            // The whole pin moves it, through the native move loop, as the
            // islands are moved by their blank space.
            const LRESULT hit = ::DefWindowProcW(hwnd_, message, wparam, lparam);
            return hit == HTCLIENT ? HTCAPTION : hit;
        }
        case WM_SETCURSOR:
            if (export_job_.running()) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_APPSTARTING));
                return TRUE;
            }
            if (LOWORD(lparam) == HTCAPTION) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        case WM_NCLBUTTONDBLCLK:
            RequestClose();
            return 0;
        case WM_NCRBUTTONDOWN:
            return 0;
        case WM_NCRBUTTONUP:
            ShowMenu({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_CONTEXTMENU:
            // From the keyboard (Shift+F10, the menu key) the point is -1, -1.
            ShowMenu(lparam == -1 ? Centre(WindowRect(hwnd_)) : POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_MOUSEWHEEL:
            Wheel(GET_WHEEL_DELTA_WPARAM(wparam), {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        case WM_KEYDOWN: {
            const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (wparam == VK_ESCAPE) {
                RequestClose();
                return 0;
            }
            if (control && wparam == 'C') {
                Copy();
                return 0;
            }
            if (control && wparam == 'S') {
                Save();
                return 0;
            }
            if (control && (wparam == '1' || wparam == VK_NUMPAD1)) {
                Zoom(1.0, Centre(WindowRect(hwnd_)));
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            switch (LOWORD(wparam)) {
                case kPinCommandCopy: Copy(); return 0;
                case kPinCommandSave: Save(); return 0;
                case kPinCommandOriginalSize: Zoom(1.0, Centre(WindowRect(hwnd_))); return 0;
                case kPinCommandClose: RequestClose(); return 0;
                case kPinCommandCloseAll: owner_.CloseAll(); return 0;
                default: break;
            }
            break;
        case WM_ACTIVATE:
            active_ = LOWORD(wparam) != WA_INACTIVE;
            InvalidateBorder();
            break;
        case WM_DPICHANGED: {
            // Windows keeps the pin's size in DIPs; the zoom follows from it.
            const RECT suggested = *reinterpret_cast<const RECT*>(lparam);
            zoom_ = PinZoomForWindow(suggested, image_size(), static_cast<float>(HIWORD(wparam)) / 96.0f);
            ::SetWindowPos(hwnd_, nullptr, suggested.left, suggested.top, suggested.right - suggested.left,
                           suggested.bottom - suggested.top, SWP_NOZORDER | SWP_NOACTIVATE);
            ::InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            KeepOnScreen();
            break;
        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) { KeepOnScreen(); }
            break;
        case WM_ERASEBKGND:
            return 1; // WM_PAINT covers every pixel.
        case WM_PAINT:
            Paint();
            return 0;
        case WM_PRINTCLIENT:
            // PrintWindow and other readers of the window's picture.
            PaintTo(reinterpret_cast<HDC>(wparam));
            return 0;
        case WM_CLOSE:
            RequestClose();
            return 0;
        default:
            if (message == theme::kChangedMessage) {
                ::InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (message == platform::kMsgImageExportCompleted) {
                OnExportCompleted(static_cast<platform::ImageExportJob::Generation>(wparam));
                return 0;
            }
            break;
    }
    return ::DefWindowProcW(hwnd_, message, wparam, lparam);
}

void PinnedImages::Window::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(hwnd_, &paint);
    if (dc == nullptr) { return; }
    PaintTo(dc);
    ::EndPaint(hwnd_, &paint);
}

void PinnedImages::Window::PaintTo(HDC dc) {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    {
        PaintBuffer buffer(dc, client);
        HDC target = buffer.dc();
        const theme::Palette& palette = theme::Current();
        theme::FillSolid(target, client, active_ ? palette.focus : palette.border);
        RECT inner = client;
        const LONG border = PinBorderPixels(WindowScale(hwnd_));
        ::InflateRect(&inner, -border, -border);
        const int width = inner.right - inner.left;
        const int height = inner.bottom - inner.top;
        // A change of activation repaints only the border: the paint target is
        // clipped to it, so a large image is not scaled again for nothing.
        if (image_ && !image_->empty() && width > 0 && height > 0 && ::RectVisible(dc, &inner) != FALSE) {
            const platform::BgraImage& image = *image_;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = image.width;
            info.bmiHeader.biHeight = -image.height; // Top-down rows.
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            if (width == image.width && height == image.height) {
                ::SetDIBitsToDevice(target, inner.left, inner.top, static_cast<DWORD>(width), static_cast<DWORD>(height), 0, 0, 0,
                                    static_cast<UINT>(image.height), image.pixels.data(), &info, DIB_RGB_COLORS);
            } else {
                // Halftone keeps a reduced pin readable; an enlarged one shows
                // its pixels sharp, so that enlarged text stays crisp.
                const bool reduced = width < image.width;
                ::SetStretchBltMode(target, reduced ? HALFTONE : COLORONCOLOR);
                if (reduced) { ::SetBrushOrgEx(target, 0, 0, nullptr); }
                ::StretchDIBits(target, inner.left, inner.top, width, height, 0, 0, image.width, image.height,
                                image.pixels.data(), &info, DIB_RGB_COLORS, SRCCOPY);
            }
        }
        buffer.Present();
    }
}

void PinnedImages::Window::InvalidateBorder() {
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    RECT inner = client;
    const LONG border = PinBorderPixels(WindowScale(hwnd_));
    ::InflateRect(&inner, -border, -border);
    const RECT edges[] = {{client.left, client.top, client.right, inner.top},
                          {client.left, inner.bottom, client.right, client.bottom},
                          {client.left, inner.top, inner.left, inner.bottom},
                          {inner.right, inner.top, client.right, inner.bottom}};
    for (const RECT& edge : edges) { ::InvalidateRect(hwnd_, &edge, FALSE); }
}

void PinnedImages::Window::Zoom(double zoom, POINT anchor) {
    if (hwnd_ == nullptr || SameZoom(zoom, zoom_)) { return; }
    const RECT next = ZoomPinAround(WindowRect(hwnd_), zoom_, zoom, image_size(), anchor, WindowScale(hwnd_));
    zoom_ = zoom;
    // A pin that grows mostly onto another monitor gets WM_DPICHANGED from
    // here, which settles its size and zoom for that monitor.
    ::SetWindowPos(hwnd_, nullptr, next.left, next.top, next.right - next.left, next.bottom - next.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    ::InvalidateRect(hwnd_, nullptr, FALSE);
}

void PinnedImages::Window::Wheel(int delta, POINT anchor) {
    if ((delta > 0) != (wheel_delta_ > 0)) { wheel_delta_ = 0; }
    wheel_delta_ += delta;
    while (hwnd_ != nullptr && std::abs(wheel_delta_) >= WHEEL_DELTA) {
        const int direction = wheel_delta_ > 0 ? 1 : -1;
        wheel_delta_ -= direction * WHEEL_DELTA;
        Zoom(NextPinZoom(zoom_, direction, image_size(), WindowScale(hwnd_)), anchor);
    }
}

void PinnedImages::Window::ShowMenu(POINT at) {
    if (hwnd_ == nullptr || busy_ != 0 || PopupActive()) { return; }
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) { return; }
    ::AppendMenuW(menu, MF_STRING, kPinCommandCopy, strings::Get(IDS_PIN_MENU_COPY));
    ::AppendMenuW(menu, MF_STRING, kPinCommandSave, strings::Get(IDS_PIN_MENU_SAVE));
    ::AppendMenuW(menu, MF_STRING | (SameZoom(zoom_, 1.0) ? MF_GRAYED : 0u), kPinCommandOriginalSize,
                  strings::Get(IDS_PIN_MENU_ORIGINAL_SIZE));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, kPinCommandClose, strings::Get(IDS_PIN_MENU_CLOSE));
    if (owner_.count() > 1) { ::AppendMenuW(menu, MF_STRING, kPinCommandCloseAll, strings::Get(IDS_PIN_MENU_CLOSE_ALL)); }
    // Required so the menu closes when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd_);
    UINT command = 0;
    ++busy_;
    {
        const PopupScope scope(hwnd_);
        const MenuThemeScope menu_theme(hwnd_, menu);
        command = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, at.x, at.y, hwnd_, nullptr);
    }
    ::DestroyMenu(menu);
    if (EndBusy()) { return; }
    ::PostMessageW(hwnd_, WM_NULL, 0, 0);
    if (command != 0) { ::PostMessageW(hwnd_, WM_COMMAND, command, 0); }
}

void PinnedImages::Window::Copy() {
    if (hwnd_ == nullptr || !image_ || image_->empty()) { return; }
    if (busy_ != 0) {
        ::MessageBeep(MB_ICONWARNING);  // A menu, a dialog or an export is still open.
        return;
    }
    static_cast<void>(StartExport(platform::ImageExportTarget::Clipboard, {}));
}

void PinnedImages::Window::Save() {
    if (hwnd_ == nullptr || !image_ || image_->empty()) { return; }
    if (busy_ != 0) {
        ::MessageBeep(MB_ICONWARNING);
        return;
    }
    ++busy_;
    std::optional<ImageSaveDestination> destination = ChooseImageDestination(hwnd_, owner_.settings_, true);
    if (EndBusy() || !destination) { return; }  // Closed while the dialog was open, or cancelled.
    static_cast<void>(StartExport(platform::ImageExportTarget::File, std::move(*destination)));
}

bool PinnedImages::Window::StartExport(platform::ImageExportTarget target, ImageSaveDestination destination) {
    platform::ImageExportRequest request{image_, target, destination.path};
    if (!export_job_.Start(hwnd_, platform::kMsgImageExportCompleted, std::move(request))) {
        // Another export of Easnap is still running.
        ::MessageBeep(MB_ICONWARNING);
        return false;
    }
    // Busy like a menu or dialog: a close asked for meanwhile waits for the result.
    ++busy_;
    export_destination_ = std::move(destination);
    const bool copy = target == platform::ImageExportTarget::Clipboard;
    export_notice_ = ShowCaptureProgressNotice(
        WindowRect(hwnd_), strings::Get(copy ? IDS_SCROLLSHOT_STATUS_COPYING : IDS_SCROLLSHOT_STATUS_SAVING));
    RefreshCursor();
    return true;
}

void PinnedImages::Window::OnExportCompleted(platform::ImageExportJob::Generation generation) {
    std::optional<platform::ImageExportResult> result = export_job_.TryTake(generation);
    if (!result) { return; }  // From an export that was dropped.
    CloseCaptureNotice(export_notice_);
    export_notice_ = nullptr;
    const ImageSaveDestination destination = std::move(export_destination_);
    export_destination_ = {};
    const RECT where = WindowRect(hwnd_);
    const bool copy = result->target == platform::ImageExportTarget::Clipboard;
    const bool succeeded = result->status == platform::ImageExportStatus::Succeeded;
    // The window still exists: busy_ holds back any close until EndBusy.
    const bool copied = copy && succeeded && platform::PublishClipboardImage(hwnd_, result->clipboard);
    if (!copy && succeeded) { RecordImageSaved(owner_.settings_, destination); }
    bool fallback = false;
    if (!copy && result->status == platform::ImageExportStatus::Failed && destination.quick && !close_requested_) {
        // The screenshots folder could not be written: the dialog offers
        // every other place for the same image.
        fallback = true;
        std::optional<ImageSaveDestination> chosen = ChooseImageDestination(hwnd_, owner_.settings_, false);
        if (chosen && !close_requested_ && StartExport(platform::ImageExportTarget::File, std::move(*chosen))) {
            static_cast<void>(EndBusy());  // The new export holds the pin busy.
            return;
        }
    }
    // The notices say what happened also when the pin closes now.
    if (copied) {
        platform::LogMessage(L"Pinned image copied to the clipboard");
        ShowCaptureCopiedNotice(where);
    } else if (!copy && succeeded) {
        if (destination.quick) {
            platform::LogMessage(L"Pinned image saved without a dialog");
            ShowCaptureSavedNotice(where);
        } else {
            platform::LogMessage(L"Pinned image saved");
        }
    } else if (result->status != platform::ImageExportStatus::Cancelled && !fallback) {
        ShowCaptureStatusNotice(where, strings::Get(copy ? IDS_EDITOR_NOTICE_CLIPBOARD_UNAVAILABLE
                                                         : IDS_EDITOR_NOTICE_SAVE_FAILED));
    }
    if (!EndBusy()) { RefreshCursor(); }  // EndBusy closes the pin when that was asked for meanwhile.
}

void PinnedImages::Window::KeepOnScreen() {
    if (hwnd_ == nullptr) { return; }
    const RECT window = WindowRect(hwnd_);
    std::vector<RECT> work_areas;
    for (const platform::MonitorInfo& monitor : platform::EnumerateMonitors()) { work_areas.push_back(monitor.work_area); }
    const RECT next = KeepPinOnScreen(window, work_areas, platform::MonitorForRect(window).work_area, WindowScale(hwnd_));
    if (::EqualRect(&next, &window) == FALSE) {
        ::SetWindowPos(hwnd_, nullptr, next.left, next.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

PinnedImages::PinnedImages(app::AppSettings* settings) : settings_(settings) {}

PinnedImages::~PinnedImages() = default; // Each pin destroys its own window.

bool PinnedImages::Pin(platform::BgraImage image, const RECT& source) {
    if (image.empty()) { return false; }
    // Called from the editor, never from a pin's own message: the objects of
    // closed pins can go now.
    pins_.erase(std::remove_if(pins_.begin(), pins_.end(), [](const auto& pin) { return pin->hwnd() == nullptr; }),
                pins_.end());
    try {
        pins_.reserve(pins_.size() + 1);
        auto pin = std::make_unique<Window>(*this, std::move(image));
        if (!pin->Create(source)) { return false; }
        pins_.push_back(std::move(pin));
    } catch (const std::bad_alloc&) {
        platform::LogMessage(L"Not enough memory to pin an image");
        return false;
    }
    platform::LogMessage(L"Image pinned (%zu open)", count());
    return true;
}

void PinnedImages::SettleExportsForSessionEnd() noexcept {
    for (const auto& pin : pins_) { pin->SettleExportForSessionEnd(); }
}

void PinnedImages::CloseAll() {
    // Index loop: closing a pin never removes an element (see `pins_`).
    for (size_t i = 0; i < pins_.size(); ++i) { pins_[i]->RequestClose(); }
}

std::vector<HWND> PinnedImages::windows() const {
    std::vector<HWND> result;
    for (const auto& pin : pins_) {
        if (pin->hwnd() != nullptr) { result.push_back(pin->hwnd()); }
    }
    return result;
}

size_t PinnedImages::count() const noexcept {
    return static_cast<size_t>(std::count_if(pins_.begin(), pins_.end(), [](const auto& pin) { return pin->hwnd() != nullptr; }));
}

} // namespace easnap::ui
