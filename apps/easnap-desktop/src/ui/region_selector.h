// Easnap - crosshair region selection.
//
// The selector is pure interaction state plus its own GDI presentation. It works
// exclusively in virtual-screen pixels so a selection can span monitors with
// different DPI values; each overlay window renders the part that falls on its
// own monitor.
//
// Presentation is plain GDI on purpose: the overlay is a layered window whose
// dimming is produced by SetLayeredWindowAttributes, which needs ordinary
// device-context drawing rather than a Direct2D swap chain.
#pragma once

#include <array>

#include "platform/platform_utils.h"

namespace easnap::ui {

/// Interactive parts of the selection rectangle.
enum class SelectionHandle {
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Body,  ///< Inside the selection: drag to move.
};

class RegionSelector {
public:
    /// Resets to "no selection yet" and stores the selectable virtual bounds.
    void Reset(const RECT& virtual_bounds);

    [[nodiscard]] bool has_selection() const noexcept { return has_selection_; }
    /// True while the selection is the non-adjustable monitor proposal shown
    /// on overlay entry (and restored by Ctrl+A).
    [[nodiscard]] bool has_implicit_selection() const noexcept { return implicit_selection_; }
    [[nodiscard]] RECT selection() const noexcept { return selection_; }
    /// Sets an adjustable, user-created selection.
    void SetSelection(RECT region);
    /// Sets the committable full-monitor proposal used on overlay entry.
    void SetImplicitSelection(RECT monitor_bounds);
    [[nodiscard]] bool interacting() const noexcept { return mode_ != Interaction::Idle; }
    [[nodiscard]] POINT cursor() const noexcept { return cursor_; }
    /// Enables interaction while the capture overlay is active.
    void set_enabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }

    /// True once the rectangle is large enough to be handed to the editor.
    /// Confirmation is always an explicit user action (Enter or double-click);
    /// releasing the mouse only ends the drag so handles can still be adjusted.
    [[nodiscard]] bool CanCommit() const noexcept { return enabled_ && has_selection_; }
    /// True when a double-click at `pt` is an explicit confirmation gesture.
    [[nodiscard]] bool CanCommitFromDoubleClick(POINT pt) const;

    // All coordinates are virtual-screen pixels.
    void OnMouseDown(POINT pt);
    void OnMouseMove(POINT pt, bool button_down);
    /// Ends the current drag. Never confirms the selection by itself.
    void OnMouseUp(POINT pt);
    /// Arrow-key nudging: moves the selection, or resizes when `resize` is set.
    void OnNudge(int dx, int dy, bool resize);

    /// `scale` is the DPI scale of the monitor under `pt`.
    [[nodiscard]] SelectionHandle HitTest(POINT pt, float scale = 1.0f) const;
    /// IDC_* cursor resource matching the handle under `pt`.
    [[nodiscard]] LPCWSTR CursorForPoint(POINT pt) const;

    /// Draws the selection border, its resize handles, the size readout and the
    /// crosshair guides for one monitor. Dimming is not drawn here: the overlay
    /// window produces it with a constant layered alpha.
    ///
    /// `monitor_rect` is in virtual-screen pixels and maps to the device
    /// context's origin; `scale` is the monitor's DPI scale so the chrome keeps
    /// a constant physical size.
    ///
    /// With `around_content` the frame is drawn outside the selection wherever
    /// the monitor has room, without handles or a size label: nothing then
    /// covers pixels that are being captured. An edge at the monitor boundary
    /// is still drawn inside. `show_label` false leaves the size readout to
    /// the caller, which may have more to say than the frame's own size.
    void RenderGdi(HDC dc, const RECT& monitor_rect, float scale, HFONT label_font,
                   bool show_handles = true, COLORREF accent = RGB(41, 162, 255),
                   bool around_content = false, bool show_label = true) const;
    /// Where the size readout of `selection` goes on a monitor, in the
    /// monitor's client coordinates, for a readout `width` pixels wide.
    [[nodiscard]] static RECT LabelRect(const RECT& selection, const RECT& monitor_rect, float scale, LONG width);

    /// Client-space damage for a selected frame: four edge/handle strips and
    /// the size label. Invalidate both old and new footprints when geometry changes.
    [[nodiscard]] static std::array<RECT, 5> GdiSelectionDamage(
        const RECT& selection, const RECT& monitor_rect, float scale);

    /// Minimum usable selection size in physical pixels.
    static constexpr LONG kMinimumSize = 8;

private:
    enum class Interaction { Idle, Creating, Moving, Resizing };

    [[nodiscard]] RECT ClampToBounds(RECT region) const;

    RECT bounds_{};
    RECT selection_{};
    RECT drag_origin_selection_{};
    POINT drag_origin_{};
    POINT cursor_{};
    Interaction mode_{Interaction::Idle};
    SelectionHandle active_handle_{SelectionHandle::None};
    bool has_selection_{false};
    bool implicit_selection_{false};
    bool enabled_{false};
};

}  // namespace easnap::ui
