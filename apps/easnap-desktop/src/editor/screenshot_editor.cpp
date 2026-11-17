// Easnap - screenshot annotation editor implementation.
#include "editor/screenshot_editor.h"

#include "editor/annotation_renderer.h"
#include "editor/annotation_tools.h"
#include "editor/editor_export.h"
#include "editor/key_commands.h"
#include "editor/toolbar_item.h"
#include "platform/graphics_device.h"
#include "platform/image_output.h"
#include "platform/screen_geometry.h"
#include "ui/island_layout.h"
#include "strings/strings.h"
#include "ui/capture_workspace.h"
#include "ui/control_ids.h"
#include "ui/image_export_outcome.h"
#include "ui/native_controls.h"
#include <commctrl.h>

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
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

/// Rows of each preview band of a document taller than kWholeBitmapRows.
constexpr int kPreviewBandRows = 4096;

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
        if (text_editor_.editing()) { text_editor_.Layout(); }
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
    toolbar_.SyncButtons(hwnd_, {ContextTool(), document_.CanUndo(), document_.CanRedo(), exporting(), has_image()}, toolbar_font_);
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
    for (HWND parent : {text_editor_.host(), context_panel_.brush_popup(), context_panel_.island(), hwnd_}) {
        if (parent && (message.hwnd == parent || ::IsChild(parent, message.hwnd))) {
            if (parent == text_editor_.host()) { return false; } // EDIT owns multiline Return, arrows and clipboard keys.
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
    text_editor_.Reset();
    pending_ = Annotation{};
    has_pending_ = false;
    dragging_ = false;
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
    text_editor_.RestorePointer();
    context_panel_.ShowBrushPopup(false);
    ::ShowWindow(hwnd_, SW_HIDE);
    if (context_panel_.island()) { ::ShowWindow(context_panel_.island(), SW_HIDE); }
    text_editor_.HideHost();
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
    if (exporting()) {
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
                text_editor_.Start(hit, point);
                text_editor_.ForwardDoubleClick(point);
            } else if (text_editor_.editing()) {
                // A blank double-click is the continuation of the first press,
                // not another creation request. It can only finish an edit.
                text_editor_.Commit();
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
    if (exporting()) {
        RefuseWhileExporting();
        return false;
    }
    if (!EnsureImage()) { return false; }
    completing_ = true;
    text_editor_.Commit();
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
    if (exporting()) {
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
    if (exporting()) {
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
    if (exporting()) {
        // Only the end of the session closes an editor during an export.
        export_.Discard();
        export_.End();
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
    export_.Discard();
    if (export_.running()) { export_.End(); }
    context_panel_.DestroyBrushPopup();
    text_editor_.DestroyHost();
    context_panel_.DestroyIsland();
    text_editor_.ReleaseFont();
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
    if (text_editor_.editing()) { text_editor_.Layout(); }
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
    if (message == WM_SETCURSOR && exporting()) { return OnBusyCursor(); }
    // The property island's timers, slider scrolls and font size list.
    if (const auto handled = context_panel_.HandleMessage(hwnd, message, wparam, lparam)) { return *handled; }
    // The typing pointer's timer and the text field's control colour.
    if (const auto handled = text_editor_.HandleMessage(message, wparam, lparam)) { return *handled; }
    const bool auxiliary = hwnd_ && hwnd != hwnd_;
    if (auxiliary) {
        if (const auto handled = OnAuxiliaryMessage(hwnd, message, wparam)) { return *handled; }
    }
    if (message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORSTATIC) {
        return ui::ControlBackground(wparam, ui::theme::Current().text);
    }
    switch (message) {
        case ui::theme::kChangedMessage: return OnThemeChanged(hwnd);
        case WM_COMMAND: OnCommand(LOWORD(wparam), HIWORD(wparam)); return 0;
        case WM_PRINTCLIENT: toolbar_.Paint(reinterpret_cast<HDC>(wparam), scale_); return 0;
        case WM_NCHITTEST: return OnNcHitTest(hwnd, lparam);
        case WM_NCLBUTTONDBLCLK: return OnGripDoubleClick(wparam);
        case WM_NCLBUTTONDOWN:
            if (OnGripPress(wparam)) { return 0; }
            break;
        case WM_ENTERSIZEMOVE: return OnEnterSizeMove(auxiliary);
        case WM_WINDOWPOSCHANGED: OnWindowPosChanged(auxiliary); break;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: return OnPaint(hwnd);
        case WM_SIZE: return OnSize(wparam, lparam);
        case WM_GETMINMAXINFO: return OnGetMinMaxInfo(lparam);
        case WM_DPICHANGED: return OnDpiChanged(hwnd, wparam, lparam);
        case WM_MOVING: return OnMoving(lparam);
        case WM_MOUSEMOVE: return OnPointerMove(hwnd, lparam);
        case WM_LBUTTONDOWN: return OnPointerDown(hwnd, lparam);
        case WM_LBUTTONDBLCLK: return OnPointerDoubleClick();
        case WM_LBUTTONUP: return OnPointerUp(hwnd, lparam);
        case WM_CAPTURECHANGED: return OnCaptureChanged();
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: return OnMouseWheel(message, wparam, lparam);
        case WM_CHAR: text_editor_.OnCharacter(static_cast<wchar_t>(wparam)); return 0;
        case WM_KEYDOWN: OnKeyDown(wparam, lparam); return 0;
        case WM_CLOSE: RequestDiscard(); return 0;
        case WM_DESTROY: return 0;
        default: break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT ScreenshotEditor::OnBusyCursor() {
    ::SetCursor(::LoadCursorW(nullptr, IDC_APPSTARTING));
    return TRUE;
}

std::optional<LRESULT> ScreenshotEditor::OnAuxiliaryMessage(HWND hwnd, UINT message, WPARAM wparam) {
    if (message == WM_SIZE || message == WM_DPICHANGED || message == WM_GETMINMAXINFO) { return 0; }
    if (message == WM_NCHITTEST) { return HTCLIENT; }
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        PAINTSTRUCT ps{};
        HDC dc = message == WM_PAINT ? ::BeginPaint(hwnd, &ps) : reinterpret_cast<HDC>(wparam);
        if (hwnd == text_editor_.host()) {
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
    return std::nullopt;
}

LRESULT ScreenshotEditor::OnThemeChanged(HWND hwnd) {
    ::RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    return 0; // No document/history/fade/placement work.
}

LRESULT ScreenshotEditor::OnNcHitTest(HWND hwnd, LPARAM lparam) {
    POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    ::ScreenToClient(hwnd, &point);
    if (::ChildWindowFromPointEx(hwnd, point, CWP_SKIPINVISIBLE) == hwnd) { return HTCAPTION; }
    return HTCLIENT;
}

LRESULT ScreenshotEditor::OnGripDoubleClick(WPARAM hit) {
    if (hit == HTCAPTION) { FeatureUsed(); }
    return 0; // Grip double-click never maximizes or completes.
}

bool ScreenshotEditor::OnGripPress(WPARAM hit) {
    // The grip is part of the floating toolbar. Fix the image before
    // Windows enters its native move loop; moving the island remains allowed.
    return hit == HTCAPTION && !FeatureUsed();
}

LRESULT ScreenshotEditor::OnEnterSizeMove(bool auxiliary) {
    if (!auxiliary) { island_user_positioned_ = true; }
    return 0;
}

void ScreenshotEditor::OnWindowPosChanged(bool auxiliary) {
    if (!auxiliary) { context_panel_.Place(); }
}

LRESULT ScreenshotEditor::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(hwnd, &ps);
    toolbar_.Paint(dc, scale_);
    ::EndPaint(hwnd, &ps);
    return 0;
}

LRESULT ScreenshotEditor::OnSize(WPARAM kind, LPARAM lparam) {
    if (kind == SIZE_MINIMIZED) {
        return 0;
    }
    const int width = static_cast<int>(LOWORD(lparam));
    const int height = static_cast<int>(HIWORD(lparam));
    LayoutClient(width, height);
    Invalidate();
    return 0;
}

LRESULT ScreenshotEditor::OnGetMinMaxInfo(LPARAM lparam) const {
    RECT minimum{0, 0, static_cast<LONG>(kMinClientWidth * scale_),
                 static_cast<LONG>(kMinClientHeight * scale_)};
    AdjustForNonClient(minimum, static_cast<UINT>(std::lround(scale_ * 96.0f)));
    auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
    info->ptMinTrackSize.x = platform::RectWidth(minimum);
    info->ptMinTrackSize.y = platform::RectHeight(minimum);
    return 0;
}

LRESULT ScreenshotEditor::OnDpiChanged(HWND hwnd, WPARAM wparam, LPARAM lparam) {
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

LRESULT ScreenshotEditor::OnMoving(LPARAM lparam) {
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

LRESULT ScreenshotEditor::OnPointerMove(HWND hwnd, LPARAM lparam) {
    const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if ((dragging_ || transforming_) && ::GetCapture() == hwnd) {
        POINT desktop = pt;
        ::ClientToScreen(hwnd, &desktop);
        UpdateStroke(desktop);
    }
    return 0;
}

LRESULT ScreenshotEditor::OnPointerDown(HWND hwnd, LPARAM lparam) {
    ::SetFocus(hwnd);
    const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    if (const auto item = toolbar_.HitTest(pt)) {
        OnToolbarActivated(*item);
    } else {
        FeatureUsed();
    } // Padding and empty toolbar space also fix the image.
    return 0;
}

LRESULT ScreenshotEditor::OnPointerDoubleClick() {
    FeatureUsed();
    return 0;
}

LRESULT ScreenshotEditor::OnPointerUp(HWND hwnd, LPARAM lparam) {
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

LRESULT ScreenshotEditor::OnCaptureChanged() {
    // Losing capture (alt-tab, resize, ...) must not leave a half stroke.
    CancelStroke();
    return 0;
}

LRESULT ScreenshotEditor::OnMouseWheel(UINT message, WPARAM wparam, LPARAM lparam) {
    // An island that holds the focus receives the wheel where Windows
    // routes it by focus. It belongs to the document all the same.
    const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    const WORD keys = GET_KEYSTATE_WPARAM(wparam);
    const bool horizontal = message == WM_MOUSEHWHEEL;
    Wheel(horizontal ? -delta : delta, (keys & MK_CONTROL) != 0, horizontal || (keys & MK_SHIFT) != 0,
          POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
    return 0;
}

void ScreenshotEditor::OnCommand(int id, int notification) {
    if (id == IDCANCEL) {
        RequestDiscard();
        return;
    }
    // The text field's EN_CHANGE.
    if (text_editor_.HandleCommand(id, notification)) { return; }
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
    if (exporting()) {
        // Nothing may change or end the image while it is copied or saved.
        if (key != VK_CONTROL && key != VK_SHIFT && key != VK_MENU) { RefuseWhileExporting(); }
        return;
    }
    if (text_editor_.ForwardKeyDown(key, flags)) { return; }
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
    if (const auto command = FindKeyCommand(key, control, shift, document_mode_)) { RunKeyCommand(*command); }
}

void ScreenshotEditor::RunKeyCommand(KeyCommand command) {
    const float page = static_cast<float>(platform::RectHeight(region_)) / zoom_ * 0.9f;
    const POINT centre{(region_.left + region_.right) / 2, (region_.top + region_.bottom) / 2};
    switch (command) {
        case KeyCommand::ScrollPageUp: ScrollDocument(0.0f, -page); return;
        case KeyCommand::ScrollPageDown: ScrollDocument(0.0f, page); return;
        case KeyCommand::ScrollToTop: ScrollDocument(0.0f, -static_cast<float>(source_image_.height)); return;
        case KeyCommand::ScrollToBottom: ScrollDocument(0.0f, static_cast<float>(source_image_.height)); return;
        case KeyCommand::ScrollUp: ScrollDocument(0.0f, -48.0f * scale_ / zoom_); return;
        case KeyCommand::ScrollDown: ScrollDocument(0.0f, 48.0f * scale_ / zoom_); return;
        case KeyCommand::ScrollLeft: ScrollDocument(-48.0f * scale_ / zoom_, 0.0f); return;
        case KeyCommand::ScrollRight: ScrollDocument(48.0f * scale_ / zoom_, 0.0f); return;
        case KeyCommand::ZoomIn: ZoomDocument(StepZoom(zoom_, 1), centre); return;
        case KeyCommand::ZoomOut: ZoomDocument(StepZoom(zoom_, -1), centre); return;
        case KeyCommand::ZoomToFitWidth: ZoomDocument(FitWidthZoom(source_image_.width, view_area_), centre); return;
        case KeyCommand::ZoomToActualSize: ZoomDocument(1.0f, centre); return;
        case KeyCommand::Undo:
        case KeyCommand::Redo: {
            // The selection is cleared even when the image cannot be fixed.
            selected_.reset();
            if (!FeatureUsed()) { return; }
            const auto revision = document_.revision();
            if (command == KeyCommand::Redo) {
                document_.Redo();
            } else {
                document_.Undo();
            }
            Invalidate();
            NotifyMutation(revision);
            return;
        }
        case KeyCommand::Copy:
            if (!FeatureUsed()) { return; }
            CompleteCopy();
            return;
        case KeyCommand::Save:
            if (!FeatureUsed()) { return; }
            SaveToFile();
            return;
        case KeyCommand::Pin:
            if (!FeatureUsed()) { return; }
            CompletePin();
            return;
        case KeyCommand::Discard: RequestDiscard(); return;
        case KeyCommand::None: return;  // No row names it; see EveryKeyBindingHasCommand.
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
    if (exporting() && !(item.kind == ToolbarItem::Kind::Command && item.command == kCommandTheme)) {
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
        text_editor_.Commit();
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
    if (exporting() || text_editor_.editing() || transforming_ || dragging_ || selected_) { return true; }
    const auto p = ToImagePoint(point);
    return tool_ != ToolId::Pointer || document_.HitTest(p, 5.0f / zoom_).has_value();
}
HCURSOR ScreenshotEditor::ImageCursor(POINT point) const {
    if (exporting()) { return ::LoadCursorW(nullptr, IDC_APPSTARTING); }
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
    if (text_editor_.editing()) {
        // Repeated clicks on the active object keep its editing session alive.
        // Only an actual outside click commits; that click never creates text.
        if (selected_ && document_.annotations()[*selected_].HitTest(ToImagePoint(point), 5.0f / zoom_)) {
            text_editor_.ForwardClick(point);
            return;
        }
        text_editor_.Commit();
        Select({});
        // Commit can remove an empty annotation and shift later indices.
        const auto hit = document_.HitTest(ToImagePoint(point), 5.0f / zoom_);
        if (hit && IsTextTool(document_.annotations()[*hit].tool)) {
            text_editor_.Start(hit, point);
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
        text_editor_.Start({}, point);
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
// ---------------------------------------------------------------------------
// Text field owner
// ---------------------------------------------------------------------------

TextEditorView ScreenshotEditor::text_view() const {
    return {view_origin_, zoom_, scale_, document_mode_ ? region_ : source_bounds_, region_};
}
HWND ScreenshotEditor::CreateTextHost(POINT point) {
    // This title identifies the text host to tests (FindWindow); it is not localized.
    return ::CreateWindowExW(kEditorExStyle, kEditorClassName, L"Edit annotation text", kEditorStyle,
                             point.x, point.y, 100, 50, hwnd_, nullptr, ::GetModuleHandleW(nullptr), this);
}
void ScreenshotEditor::BeginTextTransaction() {
    context_owns_transaction_ = false;
    document_.BeginTransaction();
}
void ScreenshotEditor::BeginMoveFromText(POINT point) {
    if (!visible_ || !selected_) { return; }
    original_ = document_.annotations()[*selected_];
    drag_origin_ = ToImagePoint(point);
    transform_handle_ = -1;
    transforming_ = true;
    context_owns_transaction_ = false;
    document_.BeginTransaction();
    ::SetCapture(hwnd_);
    ::SetFocus(hwnd_);
}
void ScreenshotEditor::TextCommitted() {
    has_pending_ = false;
    if (callbacks_.on_document_committed) { callbacks_.on_document_committed(); }
    Invalidate();
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
    return CompositeAnnotations(redaction_.image(), document_.annotations());
}
platform::BgraImage ScreenshotEditor::CompositeCommitted() {
    completing_ = true;
    text_editor_.Commit();
    completing_ = false;
    return Composite();
}

bool ScreenshotEditor::SaveToFile() {
    if (exporting()) {
        RefuseWhileExporting();
        return false;
    }
    if (!EnsureImage()) { return false; }
    text_editor_.Commit();
    // Committing the text can complete the screenshot, which copies it.
    if (!visible_ || exporting()) { return false; }
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
    if (!export_.Start(hwnd_, region_, target, std::move(image), std::move(destination))) {
        // Another export of Easnap is still running, from a pin or a Scrollshot.
        RefuseWhileExporting();
        return false;
    }
    return true;
}

void ScreenshotEditor::ExportStarted() {
    context_panel_.ShowBrushPopup(false);
    // The property island changes the image as well.
    if (context_panel_.island()) { ::EnableWindow(context_panel_.island(), FALSE); }
    SyncToolbarControls();
}

void ScreenshotEditor::ExportEnded() {
    if (context_panel_.island()) { ::EnableWindow(context_panel_.island(), TRUE); }
    SyncToolbarControls();
}

void ScreenshotEditor::OnExportCompleted(platform::ImageExportJob::Generation generation) {
    std::optional<FinishedExport> finished = export_.TakeResult(generation);
    if (!finished) { return; }  // From an export that was dropped.
    platform::ImageExportResult& result = finished->result;
    std::shared_ptr<const platform::BgraImage> image = std::move(finished->image);
    const ui::ImageSaveDestination destination = std::move(finished->destination);
    ui::RefreshCursor();
    const bool published = result.status == platform::ImageExportStatus::Succeeded &&
                           result.target == platform::ImageExportTarget::Clipboard &&
                           platform::PublishClipboardImage(hwnd_, result.clipboard);
    switch (ui::ChooseImageExportOutcome(result.status, result.target, published, destination.quick, visible_)) {
        case ui::ImageExportOutcome::Copied:
            platform::LogMessage(L"Screenshot copied to the clipboard");
            ui::ShowCaptureCopiedNotice(region_);
            Close(true);
            return;
        case ui::ImageExportOutcome::Saved:
            ui::RecordImageSaved(settings_, destination);
            if (destination.quick) {
                platform::LogMessage(L"Screenshot saved without a dialog");
                ui::ShowCaptureSavedNotice(region_);
            } else {
                platform::LogMessage(L"Screenshot exported successfully");
            }
            Close(true);
            return;
        case ui::ImageExportOutcome::RetryWithDialog: {
            // The screenshots folder could not be written: the dialog offers
            // every other place for the same image.
            auto chosen = ChooseSaveDestination(false);
            if (chosen && visible_ &&
                StartExport(platform::ImageExportTarget::File, std::move(image), std::move(*chosen))) {
                return;
            }
            break;
        }
        case ui::ImageExportOutcome::CopyFailed:
            // Failure safety: the session stays open so no capture is lost.
            // Also when the image was prepared but the clipboard could not be
            // opened.
            ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_CLIPBOARD_UNAVAILABLE));
            break;
        case ui::ImageExportOutcome::SaveFailed:
            ui::ShowCaptureStatusNotice(region_, strings::Get(IDS_EDITOR_NOTICE_SAVE_FAILED));
            break;
        case ui::ImageExportOutcome::Cancelled: break;
    }
    Invalidate();
    if (callbacks_.on_export_ended) { callbacks_.on_export_ended(); }
}

void ScreenshotEditor::RefuseWhileExporting() { ::MessageBeep(MB_ICONWARNING); }

}  // namespace easnap::editor
