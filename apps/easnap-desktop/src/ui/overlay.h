// Easnap - capture overlay and mode toolbar (fades after screenshot fixation).
//
// Each monitor has a nearly transparent input plane and a separately clipped
// outside-shading plane, so the selected desktop stays live before fixation.
// Click-through chrome supplies the frame, handles, size label and notices;
// separate surfaces hold the magnifier, opaque fixed image and fading mode bar.
// All owned surfaces request capture exclusion. Unsupported systems retain the
// hide/flush/acquire/show fallback. Frozen startup pixels serve magnifier sampling.
#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "app/session_state.h"
#include "capture/capture_provider.h"
#include "platform/platform_utils.h"
#include "ui/region_selector.h"
#include "ui/capture_magnifier.h"
#include "ui/capture_workspace.h"

namespace easnap::ui {

class OverlayWindow;

/// Owns every overlay window and the shared selection state.
class OverlayController {
public:
    struct Callbacks {
        std::function<void(app::CaptureMode, const RECT&)> on_editing_changed;
        std::function<void()> on_cancelled;
        std::function<void()> on_screenshot_complete;
        std::function<void(const RECT&)> on_region_changed;
        std::function<bool()> uses_annotation_tool;
        std::function<bool(POINT)> wants_annotation_pointer;
        std::function<HCURSOR(POINT)> annotation_cursor;
        std::function<void(bool)> on_frame_interacting;
        std::function<void(UINT, POINT)> on_image_pointer;
        std::function<void(UINT, WPARAM, LPARAM)> on_editor_key;
        std::function<void(HDC, const RECT&, const RECT&)> on_paint_image;
    };

    explicit OverlayController(Callbacks callbacks);
    ~OverlayController();

    OverlayController(const OverlayController&) = delete;
    OverlayController& operator=(const OverlayController&) = delete;

    /// Creates and shows the overlay for the given frozen desktop snapshot.
    bool Show(const capture::DesktopSnapshot& snapshot, POINT initiating_pointer);
    /// Hides the windows without destroying them. Safe to call from a window
    /// procedure; the coordinator destroys the overlay afterwards.
    void Hide();
    /// Restores the existing workspace after acquiring final pixels off-screen.
    void Resume();
    void FixScreenshot();
    /// Destroys all windows and releases device resources.
    void Destroy();

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    /// Changes only the pending capture mode. The shared selector is untouched.
    void SetMode(app::CaptureMode mode);
    [[nodiscard]] app::CaptureMode mode() const noexcept { return workspace_.mode(); }
    [[nodiscard]] CaptureWorkspace& workspace() noexcept { return workspace_; }
    [[nodiscard]] const CaptureWorkspace& workspace() const noexcept { return workspace_; }
    [[nodiscard]] HWND owner_window() const noexcept;
    /// Editor/configuration popups belong above the complete presentation stack,
    /// not to the input plane beneath the shading and image windows.
    [[nodiscard]] HWND panel_owner_window() const noexcept;
    void EnterEditing();
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;
    void RegionChanged();
    void PointerDown(POINT point, bool native_double_click, bool excluded, DWORD time);
    void PointerMove(POINT point, bool button_down);
    void PointerUp(POINT point);
    void CancelPointer();
    [[nodiscard]] bool UsesAnnotationTool() const;
    [[nodiscard]] HCURSOR AnnotationCursor(POINT point) const;
    void EditorKey(UINT message, WPARAM key, LPARAM flags);
    void PaintImage(HDC dc, const RECT& bounds, const RECT& repaint) const;
    [[nodiscard]] bool MagnifierContains(POINT point) const;

    void Invalidate();
    void InvalidateImage(const RECT& desktop_damage);
    void UpdatePointer(POINT cursor);
    [[nodiscard]] POINT pointer() const noexcept { return pointer_; }
    [[nodiscard]] std::optional<COLORREF> SamplePixel(POINT cursor) const;
    [[nodiscard]] CaptureColorReadout& color_readout() noexcept { return color_readout_; }
    [[nodiscard]] const CaptureColorReadout& color_readout() const noexcept { return color_readout_; }

    // --- Called by OverlayWindow -------------------------------------------
    /// Requests Screenshot completion after an eligible content double-click.
    void CompleteScreenshot();
    void HandleCancelled();
    [[nodiscard]] RegionSelector& selector() noexcept { return selector_; }
    [[nodiscard]] const RegionSelector& selector() const noexcept { return selector_; }
    /// Transient clipboard result (empty = hidden).
    [[nodiscard]] const std::wstring& notice() const noexcept { return notice_; }
    void SetNotice(std::wstring text);

private:
    Callbacks callbacks_;
    RegionSelector selector_;
    std::vector<std::unique_ptr<OverlayWindow>> windows_;
    std::wstring notice_;
    POINT pointer_{};
    RECT last_region_{};
    bool last_region_interacting_{false};
    bool last_has_selection_{false};
    CaptureColorReadout color_readout_;
    CaptureWorkspace workspace_;
    WorkspacePointerSequence pointer_sequence_;
    bool pointer_active_{false};
    bool selection_started_{false};
    bool visible_{false};
};

}  // namespace easnap::ui
