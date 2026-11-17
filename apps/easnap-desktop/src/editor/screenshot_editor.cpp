// Easnap - screenshot annotation editor implementation.
#include "editor/screenshot_editor.h"

#include "editor/annotation_renderer.h"
#include "editor/annotation_tools.h"
#include "editor/toolbar_item.h"
#include "platform/graphics_device.h"
#include "platform/image_output.h"
#include "platform/screen_geometry.h"
#include "ui/island_layout.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/control_ids.h"
#include "ui/native_controls.h"
#include <commctrl.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <deque>
#include <cwctype>

namespace easnap::editor {
namespace {

constexpr wchar_t kEditorClassName[] = L"EasnapScreenshotEditor";

/// Native movable tool island; image planes belong to the capture workspace.
constexpr DWORD kEditorStyle = WS_POPUP | WS_CLIPCHILDREN;
constexpr DWORD kEditorExStyle = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT;

// Smallest useful window, in device-independent pixels of the client area.
constexpr float kMinClientWidth = 460.0f;
constexpr float kMinClientHeight = kToolbarRowHeight;

/// A picture up to this height is one Direct2D bitmap and one composite pass,
/// as every screenshot is. A taller document is shown and exported in bands,
/// which keeps each bitmap within what Direct2D and WIC accept.
constexpr int kWholeBitmapRows = 8192;
constexpr int kPreviewBandRows = 4096;
constexpr int kCompositeBandRows = 2048;

/// Grows `rect` by the non-client frame of `style`. Uses the per-monitor aware
/// entry point when the running build of Windows offers it (capability probing,
/// never an OS version check) and falls back to the classic API otherwise.
void AdjustForNonClient(RECT& rect, UINT dpi) {
    using AdjustForDpiFn = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const AdjustForDpiFn adjust_for_dpi = [] {
        HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
        return user32 == nullptr ? nullptr
                                 : reinterpret_cast<AdjustForDpiFn>(reinterpret_cast<void*>(
                                       ::GetProcAddress(user32, "AdjustWindowRectExForDpi")));
    }();
    if (adjust_for_dpi != nullptr && adjust_for_dpi(&rect, kEditorStyle, FALSE, kEditorExStyle, dpi) != FALSE) {
        return;
    }
    ::AdjustWindowRectEx(&rect, kEditorStyle, FALSE, kEditorExStyle);
}

}  // namespace

ScreenshotEditor::ScreenshotEditor(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

ScreenshotEditor::~ScreenshotEditor() { Destroy(); }

bool ScreenshotEditor::CreateToolbarControls() {
    // The tools island's buttons first, then the property island's: the order
    // inside each parent window is its tab and z-order.
    if (!toolbar_.CreateButtons(hwnd_, *this)) { return false; }
    if (!context_panel_.CreateButtons(toolbar_.tooltip(), *this)) { return false; }
    toolbar_.AddScrollshotDisabledTip(hwnd_);
    SyncToolbarControls();
    return true;
}

std::optional<size_t> ScreenshotEditor::WatermarkIndex() const {
    const auto& items = document_.annotations();
    const auto found = std::find_if(items.begin(), items.end(), [](const auto& a) { return a.tool == ToolId::Watermark; });
    return found != items.end() ? std::optional<size_t>(static_cast<size_t>(found - items.begin())) : std::nullopt;
}
std::optional<size_t> ScreenshotEditor::ContextAnnotation() const {
    if (selected_ && *selected_ < document_.annotations().size()) { return selected_; }
    // Esc leaves the selection, not the watermark: its field still shows the
    // watermark's text, so its text, clear button and sliders keep acting on it.
    return tool_ == ToolId::Watermark ? WatermarkIndex() : std::nullopt;
}
ToolId ScreenshotEditor::ContextTool() const {
    const auto index = ContextAnnotation();
    return index ? document_.annotations()[*index].tool : tool_;
}
AnnotationStyle ScreenshotEditor::ContextStyle() const {
    const auto index = ContextAnnotation();
    return index ? document_.annotations()[*index].style : style_;
}
void ScreenshotEditor::Select(std::optional<size_t> index) {
    selected_ = index;
    if (selected_ && *selected_ >= document_.annotations().size()) { selected_.reset(); }
    ReflowToolbar();
    Invalidate();
}
void ScreenshotEditor::ChangeStyle(const AnnotationStyle& value) {
    const auto revision = document_.revision();
    if (const auto target = ContextAnnotation()) {
        const bool shared_opacity = document_.annotations()[*target].tool == ToolId::Spotlight && document_.annotations()[*target].style.opacity != value.opacity;
        const bool own_transaction = shared_opacity && !document_.in_transaction();
        if (own_transaction) { document_.BeginTransaction(); }
        if (shared_opacity) {
            for (size_t i = 0; i < document_.annotations().size(); ++i)
                if (document_.annotations()[i].tool == ToolId::Spotlight) {
                    auto region = document_.annotations()[i];
                    region.style.opacity = value.opacity;
                    document_.Replace(i, std::move(region));
                }
        }
        auto annotation = document_.annotations()[*target];
        annotation.style = value;
        document_.Replace(*target, std::move(annotation));
        if (own_transaction) { document_.CommitTransaction(); }
        if (editing_text_) {
            pending_ = document_.annotations()[*target];
            LayoutTextEditor();
        }
    } else {
        style_ = value;
    }
    context_panel_.Sync(false);
    Invalidate();
    NotifyMutation(revision);
}
void ScreenshotEditor::CommitContextTransaction() {
    if (!context_owns_transaction_) { return; }
    context_owns_transaction_ = false;
    document_.CommitTransaction();
}
void ScreenshotEditor::OpenContextTransaction() {
    if (!document_.in_transaction()) {
        context_owns_transaction_ = true;
        document_.BeginTransaction();
    }
}
void ScreenshotEditor::CancelContextTransaction() {
    context_owns_transaction_ = false;
    document_.CancelTransaction();
}
void ScreenshotEditor::ClearWatermarkField(HWND field) {
    document_.BeginTransaction();
    ::SetWindowTextW(field, L"");
    document_.CommitTransaction();
    context_owns_transaction_ = false;
}
void ScreenshotEditor::SetWatermarkText(std::wstring text) {
    if (selected_ && *selected_ >= document_.annotations().size()) { selected_.reset(); }
    // The image has one watermark: the field edits it whether or not it
    // is selected, and creates it only when there is none.
    const auto target = ContextAnnotation();
    Annotation a;
    if (target) {
        a = document_.annotations()[*target];
    } else {
        a.tool = ToolId::Watermark;
        a.style = style_;
        a.style.opacity = 30;
        a.points = {{0, 0}, {static_cast<float>(source_image_.width), static_cast<float>(source_image_.height)}};
    }
    a.text = std::move(text);
    if (target) {
        document_.Replace(*target, std::move(a));
        selected_ = target;
    } else {
        document_.Apply(std::move(a));
        selected_ = document_.annotations().size() - 1;
    }
    context_panel_.Sync();
    Invalidate();
}
RECT ScreenshotEditor::IslandKeepClear() const { return callbacks_.keep_clear ? callbacks_.keep_clear() : RECT{}; }
SIZE ScreenshotEditor::island_group_size() const noexcept { return context_panel_.GroupSize(); }
void ScreenshotEditor::SetFrameInteracting(bool interacting) {
    if (frame_interacting_ == interacting) { return; }
    frame_interacting_ = interacting;
    if (interacting) {
        context_panel_.ShowBrushPopup(false);
        ::ShowWindow(hwnd_, SW_HIDE);
        if (context_panel_.island()) { ::ShowWindow(context_panel_.island(), SW_HIDE); }
    } else if (visible_) {
        context_panel_.Place(true);
        ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
}

LRESULT ScreenshotEditor::ForwardButtonKey(UINT message, WPARAM key, LPARAM flags) {
    return HandleMessage(hwnd_, message, key, flags);
}

void ScreenshotEditor::BrushMethodHovered() { context_panel_.OnBrushMethodHover(); }

void ScreenshotEditor::SyncToolbarControls() const {
    if (!hwnd_) { return; }
    const auto style = ContextStyle();
    toolbar_.SyncButtons(hwnd_, {ContextTool(), document_.CanUndo(), document_.CanRedo(), exporting_, has_image()}, toolbar_font_);
    context_panel_.SyncButtons(style, toolbar_font_);
}

void ScreenshotEditor::ReflowToolbar() {
    if (!hwnd_) { return; }
    BuildToolbar(static_cast<float>(toolbar_.client_width()));
    const int height = toolbar_.height();
    RECT current{};
    ::GetWindowRect(hwnd_, &current);
    const auto monitor = platform::MonitorForRect(current);
    current.bottom = current.top + height;
    current = platform::ClampRectToBounds(current, monitor.work_area);
    ::SetWindowPos(hwnd_, nullptr, current.left, current.top, current.right - current.left, current.bottom - current.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    SyncToolbarControls();
    context_panel_.Sync();
    context_panel_.Place(!island_user_positioned_);
}

bool ScreenshotEditor::ProcessDialogMessage(MSG& message) const {
    if (!visible_) { return false; }
    for (HWND parent : {text_host_, context_panel_.brush_popup(), context_panel_.island(), hwnd_}) {
        if (parent && (message.hwnd == parent || ::IsChild(parent, message.hwnd))) {
            if (parent == text_host_) { return false; } // EDIT owns multiline Return, arrows and clipboard keys.
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                if (::SendMessageW(context_panel_.Control(kFontSizeCombo), CB_GETDROPPEDSTATE, 0, 0)) {
                    ::SendMessageW(context_panel_.Control(kFontSizeCombo), CB_SHOWDROPDOWN, FALSE, 0);
                } else {
                    ::SendMessageW(hwnd_, WM_KEYDOWN, VK_ESCAPE, message.lParam);
                }
                return true;
            }
            return ui::ConsumePopupInput(message.message, message.wParam) || ui::ActivateFocusedButton(message, parent) || ::IsDialogMessageW(parent, &message) != FALSE;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

void ScreenshotEditor::BuildToolbar(float available_width) {
    toolbar_.Build(available_width, scale_);
    context_panel_.Build(available_width);
}

void ScreenshotEditor::LayoutClient(int client_width, int client_height) {
    toolbar_.Resize(client_width, client_height);
    // The rows span the client width, which BuildToolbar is given.
    BuildToolbar(static_cast<float>(toolbar_.client_width()));
    if (!document_mode_) { ResetFixedView(); }
    SyncToolbarControls();
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void ScreenshotEditor::ResetSession() {
    context_panel_.ResetSession();
    // Undo/redo is strictly per session: nothing from a previous capture may be
    // reachable once that capture is gone.
    document_.Reset();
    selected_.reset();
    transforming_ = false;
    frame_interacting_ = false;
    island_user_positioned_ = false;
    if (text_host_) {
        ::DestroyWindow(text_host_);
        text_host_ = text_edit_ = nullptr;
    }
    pending_ = Annotation{};
    has_pending_ = false;
    dragging_ = false;
    editing_text_ = false;
    redaction_revision_ = UINT64_MAX;
    redaction_.Reset();
    image_bitmap_.Reset();
    band_top_ = band_rows_ = 0;
}

void ScreenshotEditor::CreateTextFormats() {
    HFONT font = ui::theme::CreateUiFont(ui::theme::kBodyFontSize, scale_, false);
    if (font) {
        if (toolbar_font_) { ::DeleteObject(toolbar_font_); }
        toolbar_font_ = font;
        SyncToolbarControls();
    }
}

bool ScreenshotEditor::Open(const RECT& screen_region, app::AppSettings* settings, HWND owner) {
    if (platform::IsEmptyRect(screen_region)) {
        return false;
    }
    Destroy();
    BeginSession(screen_region, settings);
    return CreateWindows(screen_region, owner);
}

bool ScreenshotEditor::OpenDocument(platform::BgraImage& image, const RECT& view_area, LONG preferred_left,
                                    app::AppSettings* settings, HWND owner) {
    if (image.empty() || platform::IsEmptyRect(view_area)) {
        return false;
    }
    Destroy();
    const ImageView view = LayoutImageView(image.width, image.height, view_area, preferred_left,
                                           FitWidthZoom(image.width, view_area), 0.0f, 0.0f);
    BeginSession(view.viewport, settings);
    document_mode_ = true;
    view_area_ = view_area;
    view_preferred_left_ = preferred_left;
    // The windows come first: only an editor that opened may take the image.
    if (!CreateWindows(view.viewport, owner)) {
        return false;
    }
    source_image_ = std::move(image);
    image = platform::BgraImage{};
    redaction_revision_ = UINT64_MAX;
    ApplyView(view);
    return true;
}

void ScreenshotEditor::BeginSession(const RECT& frame, app::AppSettings* settings) {
    source_bounds_ = frame;
    region_ = frame;
    settings_ = settings;
    scale_ = platform::MonitorForRect(frame).scale();
    ResetSession();
    closed_reported_ = false;
    tool_ = ToolId::Pointer;
    style_ = AnnotationStyle{};
    style_.thickness = kThicknessOptions[1];
    style_.brush_size = kBrushSizeOptions[1];
    style_.font_size = 16.0f;
    style_.image_scale = scale_;
}

bool ScreenshotEditor::CreateWindows(const RECT& screen_region, HWND owner) {
    static bool class_registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.lpfnWndProc = &ScreenshotEditor::WindowProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = platform::LoadAppIcon(::GetSystemMetrics(SM_CXICON));
        wc.hIconSm = platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON));
        wc.lpszClassName = kEditorClassName;
        return ::RegisterClassExW(&wc) != 0;
    }();
    if (!class_registered) {
        return false;
    }

    BuildToolbar(std::min(kIslandWidth * scale_,
                          static_cast<float>(platform::RectWidth(platform::MonitorForRect(screen_region).work_area))));
    const RECT window = toolbar_.InitialWindowRect(screen_region, scale_);
    hwnd_ = ::CreateWindowExW(kEditorExStyle, kEditorClassName, strings::Get(IDS_EDITOR_WINDOW_TITLE), kEditorStyle,
                              static_cast<int>(window.left), static_cast<int>(window.top),
                              static_cast<int>(platform::RectWidth(window)),
                              static_cast<int>(platform::RectHeight(window)), owner, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        platform::LogMessage(L"CreateWindowEx(editor) failed (error %lu)", ::GetLastError());
        return false;
    }

    RECT client{};
    ::GetClientRect(hwnd_, &client);
    LayoutClient(static_cast<int>(platform::RectWidth(client)), static_cast<int>(platform::RectHeight(client)));

    context_panel_.AdoptIsland(::CreateWindowExW(kEditorExStyle | WS_EX_LAYERED, kEditorClassName, strings::Get(IDS_EDITOR_CONTEXT_TITLE), kEditorStyle, window.left, window.bottom + 8,
                                                 static_cast<int>(window.right - window.left), 48, hwnd_, nullptr, ::GetModuleHandleW(nullptr), this));
    // This title identifies the pop-up to tests (FindWindow); it is not localized.
    context_panel_.AdoptBrushPopup(::CreateWindowExW(kEditorExStyle, kEditorClassName, L"Brush sizes", kEditorStyle, window.left, window.bottom,
                                                     static_cast<int>(132 * scale_), static_cast<int>((kToolbarRowHeight + 6) * scale_), hwnd_, nullptr, ::GetModuleHandleW(nullptr), this));
    if (context_panel_.brush_popup()) { static_cast<void>(ui::ExcludeFromCapture(context_panel_.brush_popup())); }
    if (!context_panel_.island() || !context_panel_.brush_popup() || !CreateToolbarControls()) {
        Destroy();
        return false;
    }
    context_panel_.CreateControls();
    if (!context_panel_.Control(kFontSizeCombo) || !context_panel_.Control(kEffectStrengthSlider) || !context_panel_.Control(kWatermarkTextEdit)) {
        Destroy();
        return false;
    }
    // The property island is layered already: it fades in.
    ::SetLayeredWindowAttributes(context_panel_.island(), 0, 255, LWA_ALPHA);
    static_cast<void>(ui::ExcludeFromCapture(context_panel_.island()));
    static_cast<void>(ui::ExcludeFromCapture(hwnd_));
    CreateTextFormats();
    RebuildRedactedBase();

    Show();
    ReflowToolbar();
    return true;
}

bool ScreenshotEditor::SetImage(platform::BgraImage image) {
    if (has_image() || image.empty() || image.width != platform::RectWidth(region_) ||
        image.height != platform::RectHeight(region_)) { return false; }
    source_image_ = std::move(image);
    source_bounds_ = region_;
    ResetFixedView();
    redaction_revision_ = UINT64_MAX;
    Invalidate();
    return true;
}

void ScreenshotEditor::Show() {
    visible_ = true;
    ::ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    context_panel_.Place();
    Invalidate();
}

void ScreenshotEditor::Hide() {
    visible_ = false;
    typing_pointer_.Restore();
    context_panel_.ShowBrushPopup(false);
    ::ShowWindow(hwnd_, SW_HIDE);
    if (context_panel_.island()) { ::ShowWindow(context_panel_.island(), SW_HIDE); }
    if (text_host_) { ::ShowWindow(text_host_, SW_HIDE); }
}

void ScreenshotEditor::SetRegion(const RECT& region, bool interacting) {
    if (has_image() || platform::IsEmptyRect(region)) { return; }
    if (!::EqualRect(&region_, &region)) { island_user_positioned_ = false; }
    region_ = region;
    SetFrameInteracting(interacting);
    if (!interacting) { context_panel_.Place(true); }
}

bool ScreenshotEditor::EnsureImage() {
    if (has_image()) { return true; }
    if (callbacks_.on_prepare_image && callbacks_.on_prepare_image() && has_image()) { return true; }
    ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_CAPTURE_FAILED));
    Invalidate();
    return false;
}

bool ScreenshotEditor::FeatureUsed() {
    if (!EnsureImage()) { return false; }
    if (callbacks_.on_feature_used) { callbacks_.on_feature_used(); }
    return true;
}

void ScreenshotEditor::NotifyMutation(uint64_t previous_revision) {
    if (document_.revision() != previous_revision && callbacks_.on_document_committed) {
        callbacks_.on_document_committed();
    }
}

void ScreenshotEditor::ImagePointer(UINT message, POINT point) {
    if (!visible_ || modal_active_) { return; }
    if (exporting_) {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) { RefuseWhileExporting(); }
        return;
    }
    switch (message) {
        case WM_LBUTTONDOWN:
            if (FeatureUsed()) { BeginStroke(point); }
            break;
        case WM_LBUTTONDBLCLK: {
            if (!FeatureUsed()) { break; }
            const auto hit = document_.HitTest(ToImagePoint(point), 5.0f / zoom_);
            if (hit && IsTextTool(document_.annotations()[*hit].tool)) {
                CancelStroke();
                StartTextEditing(hit, point);
                if (text_edit_) {
                    POINT local = point;
                    ::ScreenToClient(text_edit_, &local);
                    ::SendMessageW(text_edit_, WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(local.x, local.y));
                }
            } else if (editing_text_) {
                // A blank double-click is the continuation of the first press,
                // not another creation request. It can only finish an edit.
                CommitPendingText();
                Select({});
            } else if (!IsTextTool(tool_)) {
                BeginStroke(point);
            }
            break;
        }
        case WM_MOUSEMOVE: UpdateStroke(point); break;
        case WM_LBUTTONUP: EndStroke(point); break;
        case WM_CAPTURECHANGED: CancelStroke(); break;
        default: break;
    }
}

void ScreenshotEditor::Keyboard(UINT message, WPARAM key, LPARAM flags) {
    if (visible_ && !modal_active_) { HandleMessage(hwnd_, message, key, flags); }
}

bool ScreenshotEditor::CompleteCopy() {
    if (exporting_) {
        RefuseWhileExporting();
        return false;
    }
    if (!EnsureImage()) { return false; }
    completing_ = true;
    CommitPendingText();
    completing_ = false;
    // Composited here: the worker never sees the editor's own pixels.
    platform::BgraImage image = Composite();
    if (image.empty()) {
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_NOTHING_TO_COPY));
        Invalidate();
        return false;
    }
    return StartExport(platform::ImageExportTarget::Clipboard,
                       std::make_shared<const platform::BgraImage>(std::move(image)), {});
}

bool ScreenshotEditor::CompletePin() {
    if (exporting_) {
        RefuseWhileExporting();
        return false;
    }
    if (!callbacks_.on_pin || !EnsureImage()) { return false; }
    platform::BgraImage image = CompositeCommitted();
    if (image.empty()) {
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_PIN_FAILED));
        Invalidate();
        return false;
    }
    if (!callbacks_.on_pin(std::move(image), region_)) {
        // Failure safety: the session stays open so no capture is lost.
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_PIN_FAILED));
        Invalidate();
        return false;
    }
    platform::LogMessage(L"Screenshot pinned to the screen");
    Close(true);
    return true;
}

void ScreenshotEditor::RequestDiscard() {
    if (modal_active_) { return; }  // Already asking, or a file dialog is open.
    if (exporting_) {
        // The image is being copied or saved; the editor closes when that ends.
        RefuseWhileExporting();
        return;
    }
    if ((has_annotations() || document_mode_) && callbacks_.on_confirm_discard) {
        // The workspace stays enabled under the question; it must not draw.
        modal_active_ = true;
        const bool discard = callbacks_.on_confirm_discard();
        modal_active_ = false;
        if (!discard) { return; }
    }
    Close(false);
}

void ScreenshotEditor::Close(bool exported) {
    if (exporting_) {
        // Only the end of the session closes an editor during an export.
        export_job_.Discard();
        EndExport();
    }
    Hide();
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, SW_HIDE);
    }
    visible_ = false;
    // The editing session is over: drop the whole command history before anyone
    // can reopen the window, so undo/redo never leaks across captures.
    ResetSession();
    if (!closed_reported_) {
        closed_reported_ = true;
        if (callbacks_.on_closed) {
            callbacks_.on_closed(exported);
        }
    }
}

void ScreenshotEditor::Destroy() {
    export_job_.Discard();
    if (exporting_) { EndExport(); }
    context_panel_.DestroyBrushPopup();
    if (text_host_) {
        ::DestroyWindow(text_host_);
        text_host_ = text_edit_ = nullptr;
    }
    context_panel_.DestroyIsland();
    if (text_font_) {
        ::DeleteObject(text_font_);
        text_font_ = nullptr;
    }
    ResetSession();

    toolbar_.DestroyTooltip();
    image_target_.Reset();
    image_bitmap_.Reset();
    image_brush_.Reset();
    if (hwnd_ != nullptr) {
        ::SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (toolbar_font_) {
        ::DeleteObject(toolbar_font_);
        toolbar_font_ = nullptr;
    }
    source_image_ = platform::BgraImage{};
    document_mode_ = false;
    visible_ = false;
}

void ScreenshotEditor::Invalidate() const {
    SyncToolbarControls();
    if (has_image() && callbacks_.on_image_invalidated) { callbacks_.on_image_invalidated(region_); }
}

RECT ScreenshotEditor::DesktopDamage(const RECT& source_damage) const {
    if (::IsRectEmpty(&source_damage)) { return {}; }
    // A reduced view filters the picture, so a changed image pixel also shows
    // in the screen pixels of its neighbours.
    const float bleed = zoom_ < 1.0f ? 1.0f : 0.0f;
    // Image pixels to desktop pixels, rounded outwards.
    const RECT desktop_damage{
        static_cast<LONG>(std::floor(view_origin_.x + (static_cast<float>(source_damage.left) - bleed) * zoom_)),
        static_cast<LONG>(std::floor(view_origin_.y + (static_cast<float>(source_damage.top) - bleed) * zoom_)),
        static_cast<LONG>(std::ceil(view_origin_.x + (static_cast<float>(source_damage.right) + bleed) * zoom_)),
        static_cast<LONG>(std::ceil(view_origin_.y + (static_cast<float>(source_damage.bottom) + bleed) * zoom_))};
    return platform::IntersectRects(desktop_damage, region_);
}

void ScreenshotEditor::InvalidateImage(const RECT& source_damage) const {
    if (!has_image() || !callbacks_.on_image_invalidated) { return; }
    const RECT damage = DesktopDamage(source_damage);
    if (!platform::IsEmptyRect(damage)) { callbacks_.on_image_invalidated(damage); }
}

void ScreenshotEditor::ResetFixedView() {
    zoom_ = 1.0f;
    view_origin_ = D2D1::Point2F(static_cast<float>(source_bounds_.left), static_cast<float>(source_bounds_.top));
    view_ = ImageView{};
    view_.viewport = source_bounds_;
    view_.image_width = source_image_.width;
    view_.image_height = source_image_.height;
}

void ScreenshotEditor::ApplyView(const ImageView& view, bool notify) {
    const bool moved = ::EqualRect(&view.viewport, &region_) == FALSE;
    view_ = view;
    zoom_ = view.zoom;
    view_origin_ = view.origin();
    region_ = view.viewport;
    if (editing_text_) { LayoutTextEditor(); }
    // The workspace moves its image plane before anything is repainted into it.
    if (notify && callbacks_.on_view_changed) { callbacks_.on_view_changed(view_); }
    if (moved && hwnd_ && !island_user_positioned_) { context_panel_.Place(true); }
    Invalidate();
}

void ScreenshotEditor::ScrollDocument(float delta_x, float delta_y) {
    if (!document_mode_ || !has_image()) { return; }
    const ImageView next = LayoutImageView(source_image_.width, source_image_.height, view_area_, view_preferred_left_,
                                           zoom_, view_.scroll_x + delta_x, view_.scroll_y + delta_y);
    if (next.scroll_x != view_.scroll_x || next.scroll_y != view_.scroll_y) { ApplyView(next); }
}

void ScreenshotEditor::ZoomDocument(float zoom, POINT anchor) {
    if (!document_mode_ || !has_image()) { return; }
    // Zoom around a point of the picture: the pointer when it is over the
    // viewport, otherwise the viewport's centre.
    if (::PtInRect(&region_, anchor) == FALSE) {
        anchor = POINT{(region_.left + region_.right) / 2, (region_.top + region_.bottom) / 2};
    }
    const ImageView next = ZoomImageView(view_, view_area_, view_preferred_left_, zoom, anchor);
    if (next.zoom != view_.zoom) { ApplyView(next); }
}

void ScreenshotEditor::Wheel(int delta, bool control, bool shift, POINT desktop_point) {
    if (!document_mode_ || !visible_ || !has_image() || dragging_ || transforming_ || delta == 0) { return; }
    if (control) {
        ZoomDocument(StepZoom(zoom_, delta > 0 ? 1 : -1), desktop_point);
        return;
    }
    // One wheel notch moves 96 DIP of the screen, whatever the scale.
    const float step = 96.0f * scale_ / zoom_ * (static_cast<float>(delta) / static_cast<float>(WHEEL_DELTA));
    if (shift) {
        ScrollDocument(-step, 0.0f);
    } else {
        ScrollDocument(0.0f, -step);
    }
}

platform::BgraImage ScreenshotEditor::TakeImage() {
    platform::BgraImage image = std::move(source_image_);
    source_image_ = platform::BgraImage{};
    return image;
}

RECT ScreenshotEditor::AnnotationDamage(const Annotation& annotation, bool selected) const {
    const auto bounds = annotation.bounds();
    // Stroke ends, arrow heads and the mosaic brush reach past the points.
    float pad = std::max(annotation.style.brush_size, annotation.style.thickness * 6.0f) + 3.0f;
    // Glyphs overhang their layout box, and outlined text is stroked around it.
    if (IsTextTool(annotation.tool)) { pad += annotation.FontPixels(); }
    // The selection frame and its handles keep their size on screen.
    if (selected) { pad += kSelectionReach * scale_ / zoom_; }
    // Antialiasing reaches one screen pixel further.
    pad += 1.0f / zoom_;
    return RECT{static_cast<LONG>(std::floor(bounds.left - pad)), static_cast<LONG>(std::floor(bounds.top - pad)),
                static_cast<LONG>(std::ceil(bounds.right + pad)), static_cast<LONG>(std::ceil(bounds.bottom + pad))};
}

RECT ScreenshotEditor::PendingDamage() const {
    if (pending_.tool == ToolId::Text) { return RECT{0, 0, source_image_.width, source_image_.height}; }
    return AnnotationDamage(pending_, false);
}

// ---------------------------------------------------------------------------
// Message handling
// ---------------------------------------------------------------------------

LRESULT CALLBACK ScreenshotEditor::WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<ScreenshotEditor*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr) {
        return self->HandleMessage(hwnd, message, wparam, lparam);
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ScreenshotEditor::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (ui::ConsumePopupInput(message, wparam)) { return 0; }
    if (message == platform::kMsgImageExportCompleted && hwnd == hwnd_) {
        OnExportCompleted(static_cast<platform::ImageExportJob::Generation>(wparam));
        return 0;
    }
    if (message == WM_SETCURSOR && exporting_) {
        ::SetCursor(::LoadCursorW(nullptr, IDC_APPSTARTING));
        return TRUE;
    }
    // The property island's timers, slider scrolls and font size list.
    if (const auto handled = context_panel_.HandleMessage(hwnd, message, wparam, lparam)) { return *handled; }
    if (message == WM_TIMER && wparam == ui::kTypingPointerTimer) {
        typing_pointer_.Poll();
        return 0;
    }
    const bool auxiliary = hwnd_ && hwnd != hwnd_;
    if (auxiliary) {
        if (message == WM_SIZE || message == WM_DPICHANGED || message == WM_GETMINMAXINFO) { return 0; }
        if (message == WM_NCHITTEST) { return HTCLIENT; }
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT ps{};
            HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
            if (hwnd == text_host_) {
                RECT bounds{};
                ::GetWindowRect(hwnd, &bounds);
                RenderImage(dc, bounds);
            } else {
                context_panel_.Paint(dc, hwnd);
            }
            if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
            return 0;
        }
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE || message == WM_CAPTURECHANGED) { return 0; }
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC) {
        if (text_edit_ && reinterpret_cast<HWND>(lparam) == text_edit_) {
            // EDIT can paint synchronously during input while WS_VISIBLE is
            // retained for its native caret. Its GDI glyphs/backing are never
            // the annotation presentation: only PaintTextEditor owns those.
            // This DC belongs to the native draw and is reset on release; the
            // custom paint pass obtains its own unrestricted client DC.
            ::IntersectClipRect(reinterpret_cast<HDC>(wparam), 0, 0, 0, 0);
            return reinterpret_cast<LRESULT>(::GetStockObject(NULL_BRUSH));
        }
        return ui::ControlBackground(wparam, ui::theme::Current().text);
    }
    switch (message) {
        case ui::theme::kChangedMessage:
            ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
            return 0; // No document/history/fade/placement work.
        case WM_COMMAND:
            OnCommand(LOWORD(wparam), HIWORD(wparam));
            return 0;
        case WM_PRINTCLIENT:
            toolbar_.Paint(reinterpret_cast<HDC>(wparam), scale_);
            return 0;
        case WM_NCHITTEST: {
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ::ScreenToClient(hwnd, &point);
            if (::ChildWindowFromPointEx(hwnd, point, CWP_SKIPINVISIBLE) == hwnd) { return HTCAPTION; }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDBLCLK:
            if (wparam == HTCAPTION) { FeatureUsed(); }
            return 0; // Grip double-click never maximizes or completes.

        case WM_NCLBUTTONDOWN:
            // The grip is part of the floating toolbar. Fix the image before
            // Windows enters its native move loop; moving the island remains allowed.
            if (wparam == HTCAPTION && !FeatureUsed()) { return 0; }
            break;

        case WM_ENTERSIZEMOVE:
            if (!auxiliary) { island_user_positioned_ = true; }
            return 0;
        case WM_WINDOWPOSCHANGED:
            if (!auxiliary) { context_panel_.Place(); }
            break;
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = ::BeginPaint(hwnd, &ps);
            toolbar_.Paint(dc, scale_);
            ::EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_SIZE: {
            if (wparam == SIZE_MINIMIZED) {
                return 0;
            }
            const int width = static_cast<int>(LOWORD(lparam));
            const int height = static_cast<int>(HIWORD(lparam));
            LayoutClient(width, height);
            Invalidate();
            return 0;
        }

        case WM_GETMINMAXINFO: {
            RECT minimum{0, 0, static_cast<LONG>(kMinClientWidth * scale_),
                         static_cast<LONG>(kMinClientHeight * scale_)};
            AdjustForNonClient(minimum, static_cast<UINT>(std::lround(scale_ * 96.0f)));
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = platform::RectWidth(minimum);
            info->ptMinTrackSize.y = platform::RectHeight(minimum);
            return 0;
        }

        case WM_DPICHANGED: {
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            CreateTextFormats();
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) {
                const auto monitor = platform::MonitorForRect(*suggested);
                RECT available = monitor.work_area;
                available.top = std::max(available.top, monitor.bounds.top + ui::theme::Scaled(
                                                                                 ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, scale_));
                const LONG width = std::min(platform::RectWidth(available), static_cast<LONG>(kIslandWidth * scale_));
                BuildToolbar(static_cast<float>(width));
                const LONG height = toolbar_.height();
                const RECT placement = platform::ClampRectToBounds(
                    RECT{suggested->left, suggested->top, suggested->left + width, suggested->top + height}, available);
                ::SetWindowPos(hwnd, nullptr, placement.left, placement.top, platform::RectWidth(placement),
                               platform::RectHeight(placement), SWP_NOZORDER | SWP_NOACTIVATE);
            }
            // CreateTextFormats replaced the shared font; the context island
            // still holds the deleted handle until its controls are resynced.
            context_panel_.Sync();
            Invalidate();
            return 0;
        }

        case WM_MOVING: {
            auto* proposed = reinterpret_cast<RECT*>(lparam);
            if (proposed) {
                const auto monitor = platform::MonitorForRect(*proposed);
                RECT available = monitor.work_area;
                available.top = std::max(available.top, monitor.bounds.top +
                                                            ui::theme::Scaled(ui::theme::kPaletteButtonHeight + 2 * ui::theme::kPalettePadding, monitor.scale()));
                *proposed = platform::ClampRectToBounds(*proposed, available);
            }
            return TRUE;
        }

        case WM_MOUSEMOVE: {
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if ((dragging_ || transforming_) && ::GetCapture() == hwnd) {
                POINT desktop = pt;
                ::ClientToScreen(hwnd, &desktop);
                UpdateStroke(desktop);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            ::SetFocus(hwnd);
            const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            if (const auto item = toolbar_.HitTest(pt)) {
                OnToolbarActivated(*item);
            } else {
                FeatureUsed();
            } // Padding and empty toolbar space also fix the image.
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            FeatureUsed();
            return 0;

        case WM_LBUTTONUP: {
            // Finish the stroke *before* releasing capture: ReleaseCapture sends
            // WM_CAPTURECHANGED synchronously, which cancels an unfinished one.
            if (dragging_ || transforming_) {
                POINT desktop{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ::ClientToScreen(hwnd, &desktop);
                EndStroke(desktop);
            }
            if (::GetCapture() == hwnd) {
                ::ReleaseCapture();
            }
            return 0;
        }

        case WM_CAPTURECHANGED:
            // Losing capture (alt-tab, resize, ...) must not leave a half stroke.
            CancelStroke();
            return 0;

        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // An island that holds the focus receives the wheel where Windows
            // routes it by focus. It belongs to the document all the same.
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            const WORD keys = GET_KEYSTATE_WPARAM(wparam);
            const bool horizontal = message == WM_MOUSEHWHEEL;
            Wheel(horizontal ? -delta : delta, (keys & MK_CONTROL) != 0, horizontal || (keys & MK_SHIFT) != 0,
                  POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;
        }

        case WM_CHAR:
            OnCharacter(static_cast<wchar_t>(wparam));
            return 0;

        case WM_KEYDOWN:
            OnKeyDown(wparam, lparam);
            return 0;

        case WM_CLOSE:
            RequestDiscard();
            return 0;

        case WM_DESTROY:
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

void ScreenshotEditor::OnCommand(int id, int notification) {
    if (id == IDCANCEL) {
        RequestDiscard();
        return;
    }
    if (id == kInlineTextEdit && notification == EN_CHANGE) {
        UpdateTextFromControl();
        return;
    }
    // The property island's font size box, watermark field and clear button.
    if (context_panel_.HandleCommand(id, notification) || context_panel_.syncing()) { return; }
    if (notification == BN_CLICKED) {
        if (const auto item = toolbar_.Find(id)) {
            OnToolbarActivated(*item);
            return;
        }
        if (const auto item = context_panel_.Find(id)) { OnToolbarActivated(*item); }
    }
}

void ScreenshotEditor::OnKeyDown(WPARAM key, LPARAM flags) {
    if (exporting_) {
        // Nothing may change or end the image while it is copied or saved.
        if (key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU) { RefuseWhileExporting(); }
        return;
    }
    if (editing_text_ && text_edit_) {
        ::SendMessageW(text_edit_, WM_KEYDOWN, key, flags);
        return;
    }
    if (key == VK_ESCAPE && (transforming_ || dragging_ || document_.in_transaction())) {
        // The rollback can remove the selected annotation (a watermark created in this transaction).
        CancelStroke();
        document_.CancelTransaction();
        if (selected_ && *selected_ >= document_.annotations().size()) { selected_.reset(); }
        // In the watermark field Esc reverts what was typed since the field
        // took the focus. The focus stays there, so the next Esc reverts what
        // is typed after this one, however often it is pressed.
        if (context_owns_transaction_ && ::GetFocus() == context_panel_.Control(kWatermarkTextEdit) && PropertiesOf(ContextTool()).watermark) {
            document_.BeginTransaction();
        } else {
            context_owns_transaction_ = false;
        }
        context_panel_.Sync();
        Invalidate();
        return;
    }
    if (key == VK_ESCAPE && selected_) {
        Select({});
        return;
    }
    if (key == VK_DELETE && selected_) {
        document_.Remove(*selected_);
        Select({});
        return;
    }
    if (selected_ && *selected_ < document_.annotations().size() && key >= VK_LEFT && key <= VK_DOWN) {
        auto a = document_.annotations()[*selected_];
        const float step = (::GetKeyState(VK_SHIFT) & 0x8000) ? 10.0f : 1.0f;
        for (auto& p : a.points) {
            p.x += key == VK_LEFT ? -step : key == VK_RIGHT ? step
                                                            : 0;
            p.y += key == VK_UP ? -step : key == VK_DOWN ? step
                                                         : 0;
        }
        document_.Replace(*selected_, std::move(a));
        Invalidate();
        return;
    }
    if (key == VK_TAB) {
        ::SetFocus(::GetDlgItem(hwnd_, kToolButtons.first));
        return;
    }
    const bool control = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (document_mode_) {
        // A document larger than its viewport is moved and scaled from
        // the keyboard as well as with the wheel.
        const float page = static_cast<float>(platform::RectHeight(region_)) / zoom_ * 0.9f;
        const POINT centre{(region_.left + region_.right) / 2, (region_.top + region_.bottom) / 2};
        switch (key) {
            case VK_PRIOR: ScrollDocument(0.0f, -page); return;
            case VK_NEXT: ScrollDocument(0.0f, page); return;
            case VK_HOME: ScrollDocument(0.0f, -static_cast<float>(source_image_.height)); return;
            case VK_END: ScrollDocument(0.0f, static_cast<float>(source_image_.height)); return;
            case VK_UP: ScrollDocument(0.0f, -48.0f * scale_ / zoom_); return;
            case VK_DOWN: ScrollDocument(0.0f, 48.0f * scale_ / zoom_); return;
            case VK_LEFT: ScrollDocument(-48.0f * scale_ / zoom_, 0.0f); return;
            case VK_RIGHT: ScrollDocument(48.0f * scale_ / zoom_, 0.0f); return;
            case VK_OEM_PLUS:
            case VK_ADD:
                if (control) {
                    ZoomDocument(StepZoom(zoom_, 1), centre);
                    return;
                }
                break;
            case VK_OEM_MINUS:
            case VK_SUBTRACT:
                if (control) {
                    ZoomDocument(StepZoom(zoom_, -1), centre);
                    return;
                }
                break;
            case '0':
            case VK_NUMPAD0:
                if (control) {
                    ZoomDocument(FitWidthZoom(source_image_.width, view_area_), centre);
                    return;
                }
                break;
            case '1':
            case VK_NUMPAD1:
                if (control) {
                    ZoomDocument(1.0f, centre);
                    return;
                }
                break;
            default:
                break;
        }
    }
    if (control) {
        switch (key) {
            case 'Z': {
                selected_.reset();
                if (!FeatureUsed()) { return; }
                const auto revision = document_.revision();
                if (shift) {
                    document_.Redo();
                } else {
                    document_.Undo();
                }
                Invalidate();
                NotifyMutation(revision);
                return;
            }
            case 'Y': {
                selected_.reset();
                if (!FeatureUsed()) { return; }
                const auto revision = document_.revision();
                document_.Redo();
                Invalidate();
                NotifyMutation(revision);
                return;
            }
            case 'C':
                if (!FeatureUsed()) { return; }
                CompleteCopy();
                return;
            case 'S':
                if (!FeatureUsed()) { return; }
                SaveToFile();
                return;
            case 'T':
                if (!FeatureUsed()) { return; }
                CompletePin();
                return;
            default:
                break;
        }
    }
    if (key == VK_ESCAPE) {
        RequestDiscard();
        return;
    }
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

D2D1_POINT_2F ScreenshotEditor::ToImagePoint(POINT desktop_pt) const {
    const float zoom = zoom_ > 0.0f ? zoom_ : 1.0f;
    return D2D1::Point2F((static_cast<float>(desktop_pt.x) - view_origin_.x) / zoom,
                         (static_cast<float>(desktop_pt.y) - view_origin_.y) / zoom);
}

void ScreenshotEditor::OnToolbarActivated(ToolbarItem item) {
    if (exporting_ && !(item.kind == ToolbarItem::Kind::Command && item.command == kCommandTheme)) {
        RefuseWhileExporting();
        return;
    }
    if (item.kind == ToolbarItem::Kind::Command && item.command == kCommandCancel) {
        RequestDiscard();
        return;
    }
    if (item.kind == ToolbarItem::Kind::Command && item.command == kCommandScrollshot) {
        if (!has_image() && callbacks_.on_scrollshot) { callbacks_.on_scrollshot(); }
        return;
    }
    if (item.kind == ToolbarItem::Kind::Command && item.command == kCommandTheme) {
        const auto value = ui::theme::Preference() == app::AppearanceTheme::Light ? app::AppearanceTheme::Dark : app::AppearanceTheme::Light;
        if (callbacks_.on_appearance) {
            callbacks_.on_appearance(value);
        } else {
            ui::theme::ApplyPreference(value);
            ui::theme::NotifyWindows();
        }
        return;
    }
    if (!FeatureUsed()) { return; }
    if (item.identifier == kBrushMethodButton && ContextStyle().method == EffectMethod::Brush) {
        context_panel_.ShowBrushPopup(!::IsWindowVisible(context_panel_.brush_popup()));
        return;
    }
    if (item.kind == ToolbarItem::Kind::Command && item.command == kCommandCopy) {
        CompleteCopy();
        return;
    }
    if (item.kind == ToolbarItem::Kind::Command && item.command == kCommandPin) {
        CompletePin();
        return;
    }
    const auto revision = document_.revision();
    if (item.kind == ToolbarItem::Kind::Tool || item.kind == ToolbarItem::Kind::Command) {
        context_panel_.ShowBrushPopup(false);
        CommitPendingText();
        document_.CommitTransaction();
        CancelStroke();
    }
    if (!visible_) { return; }
    auto style = ContextStyle();
    switch (item.kind) {
        case ToolbarItem::Kind::Tool:
            tool_ = item.tool;
            selected_.reset();
            if (tool_ == ToolId::Watermark) {
                const auto& items = document_.annotations();
                for (size_t i = 0; i < items.size(); ++i)
                    if (items[i].tool == ToolId::Watermark) {
                        selected_ = i;
                        break;
                    }
            }
            break;
        case ToolbarItem::Kind::Color:
            style.color = item.color;
            ChangeStyle(style);
            break;
        case ToolbarItem::Kind::Thickness:
            style.thickness = item.thickness;
            ChangeStyle(style);
            break;
        case ToolbarItem::Kind::BrushSize:
            style.brush_size = item.brush_size;
            ChangeStyle(style);
            context_panel_.ShowBrushPopup(false);
            break;
        case ToolbarItem::Kind::Option:
            if (kTextModeButtons.Contains(item.identifier)) { style.text_mode = static_cast<TextMode>(kTextModeButtons.Index(item.identifier)); }
            if (kEffectButtons.Contains(item.identifier)) { style.effect = static_cast<EffectMode>(kEffectButtons.Index(item.identifier)); }
            if (kEffectMethodButtons.Contains(item.identifier)) {
                // Application method is a new-tool default. Existing region or
                // brush geometry retains its transform contract.
                if (selected_) {
                    selected_.reset();
                    style = style_;
                }
                tool_ = ToolId::Mosaic;
                style.method = static_cast<EffectMethod>(kEffectMethodButtons.Index(item.identifier));
            }
            if (kSpotlightShapeButtons.Contains(item.identifier)) { style.shape = static_cast<SpotlightShape>(kSpotlightShapeButtons.Index(item.identifier)); }
            ChangeStyle(style);
            break;
        case ToolbarItem::Kind::Command:
            selected_.reset();
            if (item.command == kCommandUndo) { document_.Undo(); }
            if (item.command == kCommandRedo) { document_.Redo(); }
            if (item.command == kCommandClear) { document_.Clear(); }
            if (item.command == kCommandSave && SaveToFile()) { return; }
            break;
    }
    ReflowToolbar();
    Invalidate();
    NotifyMutation(revision);
    if (item.identifier == kBrushMethodButton) { context_panel_.ShowBrushPopup(true); }
}

bool ScreenshotEditor::WantsPointer(POINT point) const {
    // While exporting, a click on the image is refused here instead of
    // completing the screenshot a second time.
    if (exporting_ || editing_text_ || transforming_ || dragging_ || selected_) { return true; }
    const auto p = ToImagePoint(point);
    return tool_ != ToolId::Pointer || document_.HitTest(p, 5.0f / zoom_).has_value();
}
HCURSOR ScreenshotEditor::ImageCursor(POINT point) const {
    if (exporting_) { return ::LoadCursorW(nullptr, IDC_APPSTARTING); }
    const auto p = ToImagePoint(point);
    if (selected_ && *selected_ < document_.annotations().size()) {
        const auto handles = document_.annotations()[*selected_].Handles();
        for (size_t i = 0; i < handles.size(); ++i)
            if (std::hypot(p.x - handles[i].x, p.y - handles[i].y) <= 9 * scale_ / zoom_) {
                return ::LoadCursorW(nullptr, handles.size() == 2 ? IDC_CROSS : i % 2 == 0 ? IDC_SIZENWSE
                                                                                           : IDC_SIZENESW);
            }
    }
    const bool movable = document_.HitTest(p, 5.0f / zoom_).has_value();
    return ::LoadCursorW(nullptr, movable ? IDC_SIZEALL : CursorFor(tool_));
}
void ScreenshotEditor::BeginStroke(POINT point) {
    context_panel_.ShowBrushPopup(false);
    if (editing_text_) {
        // Repeated clicks on the active object keep its editing session alive.
        // Only an actual outside click commits; that click never creates text.
        if (selected_ && document_.annotations()[*selected_].HitTest(ToImagePoint(point), 5.0f / zoom_)) {
            if (text_edit_) {
                POINT local = point;
                ::ScreenToClient(text_edit_, &local);
                ::SendMessageW(text_edit_, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(local.x, local.y));
                ::SendMessageW(text_edit_, WM_LBUTTONUP, 0, MAKELPARAM(local.x, local.y));
            }
            return;
        }
        CommitPendingText();
        Select({});
        // Commit can remove an empty annotation and shift later indices.
        const auto hit = document_.HitTest(ToImagePoint(point), 5.0f / zoom_);
        if (hit && IsTextTool(document_.annotations()[*hit].tool)) {
            StartTextEditing(hit, point);
        }
        return;
    }
    if (!visible_) { return; }
    const auto p = ToImagePoint(point);
    transform_handle_ = -1;
    if (selected_ && *selected_ < document_.annotations().size()) {
        const auto handles = document_.annotations()[*selected_].Handles();
        for (size_t i = 0; i < handles.size(); ++i)
            if (std::hypot(p.x - handles[i].x, p.y - handles[i].y) <= 9 * scale_ / zoom_) {
                transform_handle_ = static_cast<int>(i);
                break;
            }
    }
    // Hover-hit objects are selectable and movable with every creation tool.
    // Handles appear only after an explicit selection click.
    std::optional<size_t> hit;
    if (transform_handle_ >= 0) {
        hit = selected_;
    } else {
        hit = document_.HitTest(p, 5.0f / zoom_);
    }
    if (hit) {
        Select(hit);
        original_ = document_.annotations()[*hit];
        drag_origin_ = p;
        transforming_ = true;
        context_owns_transaction_ = false;
        document_.BeginTransaction();
        return;
    }
    if (tool_ == ToolId::Watermark) { return; }
    const bool leave_text_selection = IsTextTool(tool_) && selected_ && IsTextTool(document_.annotations()[*selected_].tool);
    Select({});
    // A selected text object is still the current interaction target after a
    // select/drag. Its first outside press only leaves that state; a later
    // blank press with no active selection may create the next text object.
    if (leave_text_selection) { return; }
    if (tool_ == ToolId::Pointer) { return; }
    if (IsTextTool(tool_)) {
        StartTextEditing({}, point);
        return;
    }
    pending_ = {};
    pending_.tool = tool_;
    pending_.style = style_;
    pending_.points = {p, p};
    if (tool_ == ToolId::Spotlight)
        for (const auto& a : document_.annotations())
            if (a.tool == ToolId::Spotlight) {
                pending_.style.opacity = a.style.opacity;
                break;
            }
    has_pending_ = dragging_ = true;
    InvalidateImage(PendingDamage());
}
void ScreenshotEditor::UpdateStroke(POINT point) {
    const auto p = ToImagePoint(point);
    if (transforming_ && selected_) {
        if (*selected_ >= document_.annotations().size()) { return; }
        auto updated = original_;
        if (transform_handle_ < 0) {
            const float dx = p.x - drag_origin_.x;
            const float dy = p.y - drag_origin_.y;
            for (auto& q : updated.points) {
                q.x += dx;
                q.y += dy;
            }
        } else {
            const auto handles = original_.Handles();
            const D2D1_POINT_2F q{std::clamp(p.x, 0.0f, static_cast<float>(source_image_.width)), std::clamp(p.y, 0.0f, static_cast<float>(source_image_.height))};
            if (handles.size() == 2) {
                updated.points[transform_handle_ == 0 ? 0 : updated.points.size() - 1] = q;
            } else {
                updated.points = {handles[(static_cast<size_t>(transform_handle_) + 2) % 4], q};
            }
        }
        // Only the annotation where it was and where it is, with its
        // selection chrome, is repainted. Pixels a moved effect changes
        // elsewhere are found when the paint recomputes the effects
        // (RebuildRedactedBase). The toolbar does not change during the drag;
        // EndStroke brings it up to date.
        const size_t index = *selected_;
        const RECT before = AnnotationDamage(document_.annotations()[index], true);
        const uint64_t revision = document_.revision();
        document_.Replace(index, std::move(updated));
        if (document_.revision() == revision) { return; }
        const RECT after = AnnotationDamage(document_.annotations()[index], true);
        RECT damage{};
        ::UnionRect(&damage, &before, &after);
        InvalidateImage(damage);
        return;
    }
    if (!has_pending_ || !dragging_) { return; }
    const auto last = pending_.points.back();
    if (p.x == last.x && p.y == last.y) { return; }
    const bool freehand = IsFreehand(pending_);
    RECT damage{};
    if (freehand) {
        const float pad = std::max(pending_.style.brush_size / 2, pending_.style.thickness) + 3;
        damage = {static_cast<LONG>(std::floor(std::min(last.x, p.x) - pad)), static_cast<LONG>(std::floor(std::min(last.y, p.y) - pad)),
                  static_cast<LONG>(std::ceil(std::max(last.x, p.x) + pad)), static_cast<LONG>(std::ceil(std::max(last.y, p.y) + pad))};
        pending_.points.push_back(p);
    } else {
        const RECT before = PendingDamage();
        pending_.points.back() = p;
        const RECT after = PendingDamage();
        ::UnionRect(&damage, &before, &after);
    }
    // A spotlight that switches the darkening on changes every pixel; the
    // paint that recomputes it invalidates the rest (RebuildRedactedBase).
    InvalidateImage(damage);
}
void ScreenshotEditor::EndStroke(POINT point) {
    if (transforming_) {
        UpdateStroke(point);
        transforming_ = false;
        document_.CommitTransaction();
        // Delete or undo during the drag leaves no selected annotation to compare.
        const bool moved = selected_ && *selected_ < document_.annotations().size() && !original_.Same(document_.annotations()[*selected_]);
        if (callbacks_.on_document_committed && moved) { callbacks_.on_document_committed(); }
        Invalidate();
        return;
    }
    if (!has_pending_ || !dragging_) { return; }
    const auto revision = document_.revision();
    UpdateStroke(point);
    dragging_ = false;
    const bool freehand = IsFreehand(pending_);
    const auto box = NormalizedD2DRect(pending_.points.front(), pending_.points.back());
    if (freehand || box.right - box.left >= 2 || box.bottom - box.top >= 2) {
        document_.Apply(std::move(pending_));
        selected_.reset();
    }
    has_pending_ = false;
    ReflowToolbar();
    Invalidate();
    NotifyMutation(revision);
}
void ScreenshotEditor::CancelStroke() {
    if (transforming_) {
        document_.CancelTransaction();
        transforming_ = false;
        Invalidate();
    }
    if (!dragging_) { return; }
    const RECT damage = PendingDamage();
    dragging_ = has_pending_ = false;
    pending_ = {};
    // A cancelled spotlight takes its darkening back from the whole picture;
    // the paint that recomputes the effects invalidates what lies outside.
    InvalidateImage(damage);
}
void ScreenshotEditor::StartTextEditing(std::optional<size_t> index, POINT point) {
    if (editing_text_ && index && index == selected_) {
        ::SetFocus(text_edit_);
        return;
    }
    // Committing an empty active box erases it, shifting later hit indices.
    if (editing_text_ && index && selected_ && *selected_ < *index &&
        document_.annotations()[*selected_].text.empty()) { --*index; }
    CommitPendingText();
    context_owns_transaction_ = false;
    document_.BeginTransaction();
    if (!index) {
        Annotation a;
        a.tool = tool_ == ToolId::Tag ? ToolId::Tag : ToolId::Text;
        a.style = style_;
        a.points = {ToImagePoint(point)};
        document_.Apply(std::move(a));
        index = document_.annotations().size() - 1;
    }
    selected_ = index;
    pending_ = document_.annotations()[*index];
    editing_text_ = true;
    has_pending_ = false;
    // This title identifies the text host to tests (FindWindow); it is not localized.
    text_host_ = ::CreateWindowExW(kEditorExStyle, kEditorClassName, L"Edit annotation text", kEditorStyle,
                                   point.x, point.y, 100, 50, hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
    if (text_host_) {
        text_edit_ = ::CreateWindowExW(0, L"EDIT", pending_.text.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                       0, 0, 100, 50, text_host_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kInlineTextEdit)), ::GetModuleHandleW(nullptr), nullptr);
    }
    if (!text_edit_) {
        if (text_host_) {
            ::DestroyWindow(text_host_);
            text_host_ = nullptr;
        }
        document_.CancelTransaction();
        editing_text_ = false;
        selected_.reset();
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_TEXT_EDITOR_FAILED));
        Invalidate();
        return;
    }
    ::SendMessageW(text_edit_, EM_SETLIMITTEXT, 65535, 0);
    last_text_double_ = 0;
    ui::SetAccessibleName(text_edit_, strings::Get(IDS_EDITOR_TEXT_NAME));
    ::SetWindowSubclass(text_edit_, TextEditProc, 1, reinterpret_cast<DWORD_PTR>(this));
    static_cast<void>(ui::ExcludeFromCapture(text_host_));
    LayoutTextEditor();
    ::ShowWindow(text_host_, SW_SHOWNORMAL);
    ::SetFocus(text_edit_);
    POINT local = point;
    ::ScreenToClient(text_edit_, &local);
    const auto caret = ::SendMessageW(text_edit_, EM_CHARFROMPOS, 0, MAKELPARAM(std::max(0L, local.x), std::max(0L, local.y)));
    ::SendMessageW(text_edit_, EM_SETSEL, LOWORD(caret), LOWORD(caret));
    ReflowToolbar();
    Invalidate();
}
void ScreenshotEditor::LayoutTextEditor() {
    if (!text_host_ || !text_edit_ || !selected_) { return; }
    pending_ = document_.annotations()[*selected_];
    const auto box = pending_.bounds();
    const float em = pending_.FontPixels();
    // The native input follows the scale the text is shown at.
    const float shown_em = em * zoom_;
    if (!text_font_ || text_font_pixels_ != shown_em) {
        HFONT next = ui::theme::CreateUiFont(shown_em, 1, false);
        if (next) {
            ::SendMessageW(text_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(next), FALSE);
            if (text_font_) { ::DeleteObject(text_font_); }
            text_font_ = next;
            text_font_pixels_ = shown_em;
        }
    }
    if (reinterpret_cast<HFONT>(::SendMessageW(text_edit_, WM_GETFONT, 0, 0)) != text_font_) {
        ::SendMessageW(text_edit_, WM_SETFONT, reinterpret_cast<WPARAM>(text_font_), FALSE);
    }
    const LONG pad = ui::theme::Scaled(5, scale_);
    const LONG x = static_cast<LONG>(view_origin_.x) + static_cast<LONG>(std::floor(box.left * zoom_)) - pad;
    const LONG y = static_cast<LONG>(view_origin_.y) + static_cast<LONG>(std::floor(box.top * zoom_)) - pad;
    const LONG w = std::max(2 * pad + 8L, static_cast<LONG>(std::ceil((box.right - box.left) * zoom_)) + 2 * pad);
    const LONG h = std::max(2 * pad + 8L, static_cast<LONG>(std::ceil((box.bottom - box.top) * zoom_)) + 2 * pad);
    // Only what changed is changed: Windows takes every move, resize, z-order
    // or region change of a window for pointer movement, and shows the
    // pointer it hides while the user types. This runs on every key.
    RECT placed{};
    ::GetWindowRect(text_host_, &placed);
    const bool shown = ::IsWindowVisible(text_host_) != FALSE;
    if (!shown || placed.left != x || placed.top != y || platform::RectWidth(placed) != w || platform::RectHeight(placed) != h) {
        // Topmost when it appears; afterwards it keeps its place.
        ::SetWindowPos(text_host_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | (shown ? SWP_NOZORDER : 0));
    }
    RECT input{};
    ::GetClientRect(text_edit_, &input);
    if (input.right != w || input.bottom != h) { ::MoveWindow(text_edit_, 0, 0, w, h, FALSE); }
    // The input keeps its full layout/caret coordinates, but nothing (including
    // its selection frame) may paint beyond the capture canvas.
    RECT visible = platform::IntersectRects({x, y, x + w, y + h}, document_mode_ ? region_ : source_bounds_);
    ::OffsetRect(&visible, -x, -y);
    HRGN current = ::CreateRectRgn(0, 0, 0, 0);
    const bool has_region = current && ::GetWindowRgn(text_host_, current) != ERROR;
    if (visible.left == 0 && visible.top == 0 && visible.right == w && visible.bottom == h) {
        // Wholly on the canvas: no region at all.
        if (has_region) { ::SetWindowRgn(text_host_, nullptr, FALSE); }
    } else if (HRGN clip = ::CreateRectRgn(visible.left, visible.top, visible.right, visible.bottom)) {
        if ((has_region && ::EqualRgn(current, clip)) || !::SetWindowRgn(text_host_, clip, FALSE)) { ::DeleteObject(clip); }
    }
    if (current) { ::DeleteObject(current); }
    const float inset = TextInset(pending_);
    RECT format{pad + static_cast<LONG>((inset + TagMarkerWidth(pending_)) * zoom_), pad + static_cast<LONG>(inset * zoom_), 32760, 32760};
    ::SendMessageW(text_edit_, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
    ::InvalidateRect(text_edit_, nullptr, FALSE);
}
void ScreenshotEditor::UpdateTextFromControl() {
    if (!editing_text_ || !selected_ || !text_edit_) { return; }
    const int length = ::GetWindowTextLengthW(text_edit_);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    ::GetWindowTextW(text_edit_, value.data(), length + 1);
    value.resize(length);
    auto a = document_.annotations()[*selected_];
    a.text = std::move(value);
    document_.Replace(*selected_, a);
    pending_ = std::move(a);
    if (text_dispatch_depth_) {
        text_layout_pending_ = true;
    } else {
        LayoutTextEditor();
    }
    Invalidate();
}
LRESULT CALLBACK ScreenshotEditor::TextEditProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR reference) {
    auto* self = reinterpret_cast<ScreenshotEditor*>(reference);
    if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(hwnd, TextEditProc, id);
        return ::DefSubclassProc(hwnd, message, wparam, lparam);
    }
    if (message == WM_SHOWWINDOW) { ui::NativeRedrawGuard::VisibilityChanged(hwnd, wparam != 0); }
    // The box grows with its text, and Windows shows the pointer it hides
    // for typing at each such change: the editor keeps it hidden instead.
    if (message == WM_CHAR) { self->typing_pointer_.Typed(self->hwnd_); }
    if (message == WM_KILLFOCUS) { self->typing_pointer_.Restore(); }
    if (message == WM_SETCURSOR || message == WM_LBUTTONDOWN) {
        POINT point{};
        ::GetCursorPos(&point);
        ::ScreenToClient(hwnd, &point);
        if (message == WM_LBUTTONDOWN) { point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; }
        RECT client{};
        ::GetClientRect(hwnd, &client);
        const LONG edge = ui::theme::Scaled(4, self->scale_);
        const bool border = point.x < edge || point.y < edge || point.x >= client.right - edge || point.y >= client.bottom - edge;
        if (border) {
            if (message == WM_SETCURSOR) {
                ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            ::ClientToScreen(hwnd, &point);
            self->CommitPendingText();
            if (self->visible_ && self->selected_) {
                self->original_ = self->document_.annotations()[*self->selected_];
                self->drag_origin_ = self->ToImagePoint(point);
                self->transform_handle_ = -1;
                self->transforming_ = true;
                self->context_owns_transaction_ = false;
                self->document_.BeginTransaction();
                ::SetCapture(self->hwnd_);
                ::SetFocus(self->hwnd_);
            }
            return 0;
        }
    }
    if (message == WM_ERASEBKGND) { return 1; }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        RECT client{};
        ::GetClientRect(hwnd, &client);
        ::HideCaret(hwnd);
        {
            platform::PaintBuffer buffer(dc, client);
            self->PaintTextEditor(buffer.dc());
        }
        ::ShowCaret(hwnd);
        if (message == WM_PAINT) { ::EndPaint(hwnd, &ps); }
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE) {
        self->editing_text_ = false;
        self->document_.CancelTransaction();
        HWND host = self->text_host_;
        self->text_host_ = self->text_edit_ = nullptr;
        ::DestroyWindow(host);
        self->selected_.reset();
        self->ReflowToolbar();
        self->Invalidate();
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_RETURN && (::GetKeyState(VK_CONTROL) & 0x8000)) {
        self->CommitPendingText();
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == 'A' && (::GetKeyState(VK_CONTROL) & 0x8000)) {
        ::SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    if (message == WM_LBUTTONDBLCLK) {
        self->last_text_double_ = ::GetTickCount();
        self->last_text_point_ = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    }
    if (message == WM_LBUTTONDOWN && self->last_text_double_ && ::GetTickCount() - self->last_text_double_ <= ::GetDoubleClickTime() &&
        std::abs(GET_X_LPARAM(lparam) - self->last_text_point_.x) <= ::GetSystemMetrics(SM_CXDOUBLECLK) &&
        std::abs(GET_Y_LPARAM(lparam) - self->last_text_point_.y) <= ::GetSystemMetrics(SM_CYDOUBLECLK)) {
        const size_t index = LOWORD(::SendMessageW(hwnd, EM_CHARFROMPOS, 0, lparam));
        const auto& value = self->document_.annotations()[*self->selected_].text;
        size_t start = std::min(index, value.size()), end = start;
        while (start > 0 && value[start - 1] != L'\n') { --start; }
        while (end < value.size() && value[end] != L'\n') { ++end; }
        if (end < value.size()) { ++end; }
        ::SendMessageW(hwnd, EM_SETSEL, start, end);
        self->last_text_double_ = 0;
        return 0;
    }
    const bool changes_pixels = (message >= WM_KEYFIRST && message <= WM_KEYLAST) ||
                                (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) || message == EM_SETSEL || message == EM_REPLACESEL ||
                                message == WM_SETTEXT || message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_SIZE ||
                                message == WM_CUT || message == WM_PASTE || message == WM_CLEAR || message == WM_UNDO || message == EM_UNDO ||
                                message == WM_IME_COMPOSITION || message == WM_IME_ENDCOMPOSITION || message == WM_TIMER || message == WM_CAPTURECHANGED;
    // The native caret stays hidden for the whole dispatch. EDIT would show it
    // at its own GDI position first; it is placed against the rendered text
    // below, by which time the control is invalid, and Windows does not erase
    // a caret inside an update region. The first column would stay inverted
    // until the next paint.
    const bool caret_hidden = changes_pixels && ::HideCaret(hwnd) != FALSE;
    const auto result = [&] {
        ++self->text_dispatch_depth_;
        // Typing is not suspended. Suspending redraw takes WS_VISIBLE away and
        // gives it back, which Windows takes for pointer movement: the pointer
        // it hides while the user types came back at every key. The control
        // colour handler already keeps EDIT's own drawing off the screen.
        const bool typing = message >= WM_KEYFIRST && message <= WM_KEYLAST;
        ui::NativeRedrawGuard redraw(hwnd, changes_pixels && !typing);
        // EDIT needs visibility for native caret/selection updates, even while
        // its internal redraw flag suppresses synchronous GDI text painting.
        const LONG_PTR style = ::GetWindowLongPtrW(hwnd, GWL_STYLE);
        if (changes_pixels && (style & WS_VISIBLE) == 0) { ::SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_VISIBLE); }
        const auto result = ::DefSubclassProc(hwnd, message, wparam, lparam);
        --self->text_dispatch_depth_;
        return result;
    }();
    if (!self->text_dispatch_depth_ && self->text_layout_pending_) {
        self->text_layout_pending_ = false;
        self->LayoutTextEditor();
    }
    if (changes_pixels && !self->text_dispatch_depth_ && ::IsWindow(hwnd) && ::GetFocus() == hwnd && self->selected_) {
        DWORD start{}, end{};
        ::SendMessageW(hwnd, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        // The native caret can be parked outside the old client bounds while
        // typing grows the host. Position it against the rendered text after
        // layout; native EDIT still owns selection, navigation and caret life.
        if (start == end) {
            const auto& a = self->document_.annotations()[*self->selected_];
            auto layout = TextLayout(a);
            float x{}, y{};
            DWRITE_HIT_TEST_METRICS metrics{};
            if (layout && SUCCEEDED(layout->HitTestTextPosition(start, FALSE, &x, &y, &metrics))) {
                RECT format{};
                ::SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&format));
                ::SetCaretPos(format.left + static_cast<int>(std::lround(x * self->zoom_)), format.top + static_cast<int>(std::lround(y * self->zoom_)));
            }
        }
    }
    // Fails harmlessly when the dispatch gave the caret away.
    if (caret_hidden && ::IsWindow(hwnd)) { ::ShowCaret(hwnd); }
    return result;
}
void ScreenshotEditor::PaintTextEditor(HDC dc) {
    if (!text_edit_ || !selected_) { return; }
    RECT bounds{};
    ::GetWindowRect(text_edit_, &bounds);
    RenderImage(dc, bounds);
    DWORD start{}, end{};
    ::SendMessageW(text_edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end) { return; }
    const auto& a = document_.annotations()[*selected_];
    auto layout = TextLayout(a);
    if (!layout) { return; }
    const float inset = TextInset(a);
    // Layout units are image pixels; the host's pixels are scaled by the view.
    const float x = (view_origin_.x - bounds.left) / zoom_ + a.points.front().x + inset + TagMarkerWidth(a), y = (view_origin_.y - bounds.top) / zoom_ + a.points.front().y + inset;
    UINT32 count{};
    layout->HitTestTextRange(start, end - start, x, y, nullptr, 0, &count);
    std::vector<DWRITE_HIT_TEST_METRICS> ranges(count);
    if (SUCCEEDED(layout->HitTestTextRange(start, end - start, x, y, ranges.data(), count, &count))) {
        for (const auto& r : ranges) {
            RECT area{static_cast<LONG>(r.left * zoom_), static_cast<LONG>(r.top * zoom_), static_cast<LONG>(std::ceil((r.left + r.width) * zoom_)), static_cast<LONG>(std::ceil((r.top + r.height) * zoom_))};
            ::InvertRect(dc, &area);
        }
    }
}
void ScreenshotEditor::CommitPendingText() {
    typing_pointer_.Restore();
    if (!editing_text_) { return; }
    UpdateTextFromControl();
    editing_text_ = false;
    if (selected_ && document_.annotations()[*selected_].text.empty()) {
        document_.Remove(*selected_);
        selected_.reset();
    }
    document_.CommitTransaction();
    if (text_host_) {
        HWND host = text_host_;
        text_host_ = text_edit_ = nullptr;
        ::DestroyWindow(host);
    }
    has_pending_ = false;
    if (callbacks_.on_document_committed) { callbacks_.on_document_committed(); }
    Invalidate();
}
void ScreenshotEditor::OnCharacter(wchar_t character) {
    if (!exporting_ && editing_text_ && text_edit_) { ::SendMessageW(text_edit_, WM_CHAR, character, 0); }
}
void ScreenshotEditor::DrawSelection(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush) const {
    if (!selected_ || *selected_ >= document_.annotations().size()) { return; }
    // Frame and handles keep their size on screen at every scale of the view.
    DrawSelectionChrome(target, brush, document_.annotations()[*selected_], scale_ / zoom_);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

RECT ScreenshotEditor::RebuildRedactedBase(const RECT* painting) {
    const bool effect_preview = has_pending_ && IsBaseEffect(pending_.tool);
    if ((redaction_revision_ == document_.revision() && !effect_preview) || !has_image()) { return {}; }
    std::vector<RedactionEffect> effects;
    const auto add = [&](const Annotation& a) {
        if (!IsBaseEffect(a.tool) || a.points.empty()) { return; }
        RedactionEffect effect;
        effect.box = a.bounds();
        effect.strength = a.style.strength;
        if (a.tool == ToolId::Spotlight) {
            effect.kind = RedactionKind::Spotlight;
            effect.dim = a.style.opacity / 100;
            effect.corner_radius = 16 * a.style.image_scale;
            effect.outline = a.style.shape == SpotlightShape::Ellipse ? RedactionOutline::Ellipse : a.style.shape == SpotlightShape::Rounded ? RedactionOutline::Rounded
                                                                                                                                             : RedactionOutline::Rectangle;
        } else {
            effect.kind = a.style.effect == EffectMode::Pixelate ? RedactionKind::Pixelate : RedactionKind::Blur;
            if (a.style.method == EffectMethod::Brush) {
                effect.brush = true;
                effect.brush_path = a.points;
                effect.brush_radius = a.style.brush_size / 2;
            }
        }
        effects.push_back(std::move(effect));
    };
    for (const auto& a : document_.annotations()) { add(a); }
    if (effect_preview) { add(pending_); }
    redaction_revision_ = effect_preview ? UINT64_MAX : document_.revision();
    // Only the pixels an edit can influence are recomputed and uploaded.
    const RECT changed = redaction_.Update(source_image_, std::move(effects));
    if (::IsRectEmpty(&changed)) { return changed; }
    // The bitmap holds rows [band_top_, band_top_ + band_rows_): the whole
    // picture for a screenshot, the band on show for a tall document.
    const LONG top = std::max<LONG>(changed.top, band_top_), bottom = std::min<LONG>(changed.bottom, band_top_ + band_rows_);
    if (image_bitmap_ && bottom > top) {
        const auto& image = redaction_.image();
        const D2D1_RECT_U target{static_cast<UINT32>(changed.left), static_cast<UINT32>(top - band_top_), static_cast<UINT32>(changed.right), static_cast<UINT32>(bottom - band_top_)};
        if (FAILED(image_bitmap_->CopyFromMemory(&target, image.row(top) + changed.left, static_cast<UINT32>(image.stride_bytes())))) {
            image_bitmap_.Reset();
            band_rows_ = 0;
        }
    }
    // Whatever invalidated this paint, every changed pixel reaches the
    // screen: what the paint does not cover is invalidated for the next one,
    // which finds nothing left to recompute.
    const RECT shown = DesktopDamage(changed);
    RECT covered{};
    if (!platform::IsEmptyRect(shown) && (painting == nullptr || !::IntersectRect(&covered, &shown, painting) || !::EqualRect(&covered, &shown))) {
        InvalidateImage(changed);
    }
    return changed;
}

int ScreenshotEditor::EnsureBaseRows(int first, int end) {
    const auto& image = redaction_.image();
    if (!image_target_ || image.empty() || first < 0 || first >= image.height || end <= first) { return 0; }
    end = std::min(end, image.height);
    const int limit = static_cast<int>(std::min<UINT32>(image_target_->GetMaximumBitmapSize(), 16384u));
    if (image.height <= std::min(kWholeBitmapRows, limit)) {
        if (!image_bitmap_) {
            image_bitmap_ = platform::CreateD2DBitmapForTarget(image_target_.Get(), image);
            band_top_ = 0;
            band_rows_ = image_bitmap_ ? image.height : 0;
        }
        return image_bitmap_ ? end - first : 0;
    }
    if (image_bitmap_ && first >= band_top_ && first < band_top_ + band_rows_) {
        return std::min(end, band_top_ + band_rows_) - first;
    }
    // Centre the band on what is wanted, so scrolling a little reuses it.
    const int rows = std::min({kPreviewBandRows, limit, image.height});
    const int top = std::clamp(first - (rows - std::min(end - first, rows)) / 2, 0, image.height - rows);
    if (!image_bitmap_) {
        const D2D1_BITMAP_PROPERTIES properties = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(image_target_->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(image.width), static_cast<UINT32>(rows)),
                                               properties, &image_bitmap_))) {
            image_bitmap_.Reset();
        }
    } else {
        // Draw calls already issued still refer to the rows about to be replaced.
        image_target_->Flush();
    }
    if (!image_bitmap_ ||
        FAILED(image_bitmap_->CopyFromMemory(nullptr, image.row(top), static_cast<UINT32>(image.stride_bytes())))) {
        image_bitmap_.Reset();
        band_rows_ = 0;
        return 0;
    }
    band_top_ = top;
    band_rows_ = rows;
    return std::min(end, band_top_ + band_rows_) - first;
}

void ScreenshotEditor::RenderImage(HDC dc, const RECT& bounds, const RECT* repaint) {
    auto& graphics = platform::GraphicsEnvironment::Get();
    if (!has_image() || !graphics.EnsureInitialized() || !dc) { return; }
    RECT client{0, 0, platform::RectWidth(bounds), platform::RectHeight(bounds)};
    if (repaint) { client = platform::IntersectRects(client, *repaint); }
    RECT painting = client;
    ::OffsetRect(&painting, bounds.left, bounds.top);
    RebuildRedactedBase(&painting);
    if (!image_target_) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        if (FAILED(graphics.d2d_factory()->CreateDCRenderTarget(&properties, &image_target_))) { return; }
        image_target_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &image_brush_);
    }
    if (platform::IsEmptyRect(client) || FAILED(image_target_->BindDC(dc, &client))) { return; }
    // Where image pixel (0, 0) falls in this paint, and which image rows the
    // paint can show. A screenshot shows all of its rows.
    const float origin_x = view_origin_.x - static_cast<float>(bounds.left + client.left);
    const float origin_y = view_origin_.y - static_cast<float>(bounds.top + client.top);
    int first_row = 0;
    int end_row = source_image_.height;
    if (document_mode_) {
        first_row = std::clamp(static_cast<int>(std::floor(-origin_y / zoom_)) - 1, 0, source_image_.height);
        end_row = std::clamp(static_cast<int>(std::ceil((static_cast<float>(platform::RectHeight(client)) - origin_y) / zoom_)) + 1,
                             first_row, source_image_.height);
    }
    if (EnsureBaseRows(first_row, std::max(end_row, first_row + 1)) <= 0 || !image_bitmap_ || !image_brush_) { return; }
    image_target_->BeginDraw();
    const D2D1::Matrix3x2F translation = D2D1::Matrix3x2F::Translation(origin_x, origin_y);
    image_target_->SetTransform(zoom_ == 1.0f ? translation : D2D1::Matrix3x2F::Scale(zoom_, zoom_) * translation);
    image_target_->PushAxisAlignedClip(D2D1::RectF(0, 0, static_cast<float>(source_image_.width), static_cast<float>(source_image_.height)), D2D1_ANTIALIAS_MODE_ALIASED);
    if (band_top_ == 0 && band_rows_ == source_image_.height && zoom_ == 1.0f) {
        image_target_->DrawBitmap(image_bitmap_.Get());
    } else {
        // Reduced pictures are filtered; enlarged ones keep their pixels sharp.
        const auto filter = zoom_ > 1.0f ? D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR
                                         : D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        const float width = static_cast<float>(source_image_.width);
        for (int row = first_row; row < end_row;) {
            const int covered = EnsureBaseRows(row, end_row);
            if (covered <= 0 || !image_bitmap_) { break; }
            image_target_->DrawBitmap(image_bitmap_.Get(),
                                      D2D1::RectF(0, static_cast<float>(row), width, static_cast<float>(row + covered)), 1.0f, filter,
                                      D2D1::RectF(0, static_cast<float>(row - band_top_), width, static_cast<float>(row - band_top_ + covered)));
            row += covered;
        }
    }
    for (const auto& annotation : document_.annotations()) {
        DrawAnnotation(image_target_.Get(), annotation, image_brush_.Get(), false);
    }
    if (has_pending_ && !IsBaseEffect(pending_.tool)) {
        DrawAnnotation(image_target_.Get(), pending_, image_brush_.Get(), true);
    }
    DrawSelection(image_target_.Get(), image_brush_.Get());
    image_target_->PopAxisAlignedClip();
    if (FAILED(image_target_->EndDraw())) {
        image_bitmap_.Reset();
        image_brush_.Reset();
        image_target_.Reset();
        band_rows_ = 0;
    }
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------

platform::BgraImage ScreenshotEditor::Composite() {
    RebuildRedactedBase();

    platform::BgraImage output;
    auto& graphics = platform::GraphicsEnvironment::Get();
    const platform::BgraImage& base_image = redaction_.image();
    if (!graphics.EnsureInitialized() || base_image.empty()) {
        return output;
    }
    // Once the pixels exist the frame cannot move, so the export is always the
    // whole image: the captured region, or the stitched document.
    if (document_.annotations().empty()) {
        return base_image;  // Nothing to composite.
    }

    // Always the captured resolution - never the window or preview size. A
    // screenshot is one pass; a tall document is composited in bands, each
    // with the same annotation list shifted to its rows.
    const int width = base_image.width;
    const int height = base_image.height;
    const int band_rows = height <= kWholeBitmapRows ? height : kCompositeBandRows;
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f);
    const D2D1_BITMAP_PROPERTIES base_properties =
        D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
    output.Reset(width, height);
    for (int top = 0; top < height; top += band_rows) {
        const int rows = std::min(band_rows, height - top);
        ComPtr<IWICBitmap> wic_bitmap;
        HRESULT hr = graphics.wic_factory()->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(rows),
                                                          GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &wic_bitmap);
        if (FAILED(hr)) {
            platform::LogHresult(L"WIC CreateBitmap(composite)", hr);
            return {};
        }
        ComPtr<ID2D1RenderTarget> render_target;
        hr = graphics.d2d_factory()->CreateWicBitmapRenderTarget(wic_bitmap.Get(), properties, &render_target);
        if (FAILED(hr)) {
            platform::LogHresult(L"CreateWicBitmapRenderTarget", hr);
            return {};
        }

        ComPtr<ID2D1Bitmap> base;
        hr = render_target->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(width), static_cast<UINT32>(rows)),
                                         base_image.row(top), static_cast<UINT32>(base_image.stride_bytes()),
                                         base_properties, &base);
        if (FAILED(hr)) {
            platform::LogHresult(L"ID2D1RenderTarget::CreateBitmap", hr);
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        render_target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        if (!base || !brush) { return {}; }

        render_target->BeginDraw();
        render_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
        render_target->DrawBitmap(base.Get(),
                                  D2D1::RectF(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(rows)));
        render_target->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, -static_cast<float>(top)));
        for (const Annotation& annotation : document_.annotations()) {
            DrawAnnotation(render_target.Get(), annotation, brush.Get(), false);
        }
        hr = render_target->EndDraw();
        if (FAILED(hr)) {
            platform::LogHresult(L"Composite EndDraw", hr);
            return {};
        }

        WICRect lock_rect{0, 0, static_cast<INT>(width), static_cast<INT>(rows)};
        ComPtr<IWICBitmapLock> lock;
        hr = wic_bitmap->Lock(&lock_rect, WICBitmapLockRead, &lock);
        if (FAILED(hr)) {
            platform::LogHresult(L"IWICBitmap::Lock", hr);
            return {};
        }
        UINT stride = 0;
        UINT buffer_size = 0;
        BYTE* data = nullptr;
        if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&buffer_size, &data)) || data == nullptr) {
            return {};
        }
        for (int y = 0; y < rows; ++y) {
            std::memcpy(output.row(top + y), data + static_cast<size_t>(y) * stride, output.stride_bytes());
        }
    }
    output.MakeOpaque();
    return output;
}
platform::BgraImage ScreenshotEditor::CompositeCommitted() {
    completing_ = true;
    CommitPendingText();
    completing_ = false;
    return Composite();
}

bool ScreenshotEditor::SaveToFile() {
    if (exporting_) {
        RefuseWhileExporting();
        return false;
    }
    if (!EnsureImage()) { return false; }
    CommitPendingText();
    // Committing the text can complete the screenshot, which copies it.
    if (!visible_ || exporting_) { return false; }
    platform::BgraImage composite = Composite();
    if (composite.empty()) {
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_NOTHING_TO_SAVE));
        Invalidate();
        return false;
    }
    auto image = std::make_shared<const platform::BgraImage>(std::move(composite));
    auto destination = ChooseSaveDestination(true);
    if (!destination || !visible_) { return false; }  // Cancelled: keep editing.
    return StartExport(platform::ImageExportTarget::File, std::move(image), std::move(*destination));
}

std::optional<ui::ImageSaveDestination> ScreenshotEditor::ChooseSaveDestination(bool allow_quick) {
    bool dialog = false;
    auto destination = ui::ChooseImageDestination(hwnd_, settings_, allow_quick, [&] {
        // Topmost owned islands and combo lists must not cover a native file
        // dialog. Freeze their timers/placement during its nested message loop.
        dialog = true;
        modal_active_ = true;
        context_panel_.ShowBrushPopup(false);
        context_panel_.StopForDialog();
        toolbar_.SuspendTooltips();
        ::ShowWindow(context_panel_.island(), SW_HIDE);
    });
    if (dialog) {
        modal_active_ = false;
        toolbar_.ResumeTooltips();
        if (visible_) { context_panel_.Place(); }
    }
    return destination;
}

bool ScreenshotEditor::StartExport(platform::ImageExportTarget target, std::shared_ptr<const platform::BgraImage> image,
                                   ui::ImageSaveDestination destination) {
    platform::ImageExportRequest request{image, target, destination.path};
    if (!export_job_.Start(hwnd_, platform::kMsgImageExportCompleted, std::move(request))) {
        // Another export of Easnap is still running, from a pin or a Scrollshot.
        RefuseWhileExporting();
        return false;
    }
    exporting_ = true;
    export_image_ = std::move(image);
    export_destination_ = std::move(destination);
    const bool copy = target == platform::ImageExportTarget::Clipboard;
    export_notice_ = ui::ShowCaptureProgressNotice(
        region_, strings::Get(copy ? IDS_SCROLLSHOT_STATUS_COPYING : IDS_SCROLLSHOT_STATUS_SAVING));
    context_panel_.ShowBrushPopup(false);
    // The property island changes the image as well.
    if (context_panel_.island()) { ::EnableWindow(context_panel_.island(), FALSE); }
    SyncToolbarControls();
    ui::RefreshCursor();
    return true;
}

void ScreenshotEditor::EndExport() {
    exporting_ = false;
    ui::CloseCaptureNotice(export_notice_);
    export_notice_ = nullptr;
    export_image_.reset();
    export_destination_ = {};
    if (context_panel_.island()) { ::EnableWindow(context_panel_.island(), TRUE); }
    SyncToolbarControls();
}

void ScreenshotEditor::OnExportCompleted(platform::ImageExportJob::Generation generation) {
    std::optional<platform::ImageExportResult> result = export_job_.TryTake(generation);
    if (!result) { return; }  // From an export that was dropped.
    std::shared_ptr<const platform::BgraImage> image = export_image_;
    const ui::ImageSaveDestination destination = export_destination_;
    EndExport();
    ui::RefreshCursor();
    const bool copy = result->target == platform::ImageExportTarget::Clipboard;
    if (result->status == platform::ImageExportStatus::Succeeded) {
        if (copy && platform::PublishClipboardImage(hwnd_, result->clipboard)) {
            platform::LogMessage(L"Screenshot copied to the clipboard");
            ui::ShowCaptureCopiedNotice(region_);
            Close(true);
            return;
        }
        if (!copy) {
            ui::RecordImageSaved(settings_, destination);
            if (destination.quick) {
                platform::LogMessage(L"Screenshot saved without a dialog");
                ui::ShowCaptureSavedNotice(region_);
            } else {
                platform::LogMessage(L"Screenshot exported successfully");
            }
            Close(true);
            return;
        }
    }
    if (result->status == platform::ImageExportStatus::Failed && !copy && destination.quick && visible_) {
        // The screenshots folder could not be written: the dialog offers
        // every other place for the same image.
        auto chosen = ChooseSaveDestination(false);
        if (chosen && visible_ && StartExport(platform::ImageExportTarget::File, std::move(image), std::move(*chosen))) {
            return;
        }
    } else if (result->status == platform::ImageExportStatus::Failed) {
        // Failure safety: the session stays open so no capture is lost.
        ui::ShowCaptureStatusNotice(region_, strings::Get(copy ? IDS_EDITOR_NOTICE_CLIPBOARD_UNAVAILABLE
                                                               : IDS_EDITOR_NOTICE_SAVE_FAILED));
    } else if (result->status == platform::ImageExportStatus::Succeeded) {
        // Prepared, but the clipboard could not be opened.
        ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_CLIPBOARD_UNAVAILABLE));
    }
    Invalidate();
    if (callbacks_.on_export_ended) { callbacks_.on_export_ended(); }
}

void ScreenshotEditor::RefuseWhileExporting() { ::MessageBeep(MB_ICONWARNING); }

}  // namespace easnap::editor
