// Easnap - crosshair region selection implementation.
#include "ui/region_selector.h"

#include <algorithm>
#include <array>
#include <cstdio>

#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {

struct HandleLayout {
    SelectionHandle id;
    float x_factor;  ///< 0 = left, 0.5 = centre, 1 = right
    float y_factor;
};

constexpr std::array<HandleLayout, 8> kHandles = {{
    {SelectionHandle::TopLeft, 0.0f, 0.0f},
    {SelectionHandle::Top, 0.5f, 0.0f},
    {SelectionHandle::TopRight, 1.0f, 0.0f},
    {SelectionHandle::Right, 1.0f, 0.5f},
    {SelectionHandle::BottomRight, 1.0f, 1.0f},
    {SelectionHandle::Bottom, 0.5f, 1.0f},
    {SelectionHandle::BottomLeft, 0.0f, 1.0f},
    {SelectionHandle::Left, 0.0f, 0.5f},
}};

POINT HandleCenter(const RECT& selection, const HandleLayout& layout) {
    const float width = static_cast<float>(platform::RectWidth(selection));
    const float height = static_cast<float>(platform::RectHeight(selection));
    return POINT{selection.left + static_cast<LONG>(width * layout.x_factor),
                 selection.top + static_cast<LONG>(height * layout.y_factor)};
}

/// Translates a virtual-screen rectangle into the device context of one monitor.
RECT ToClientRect(const RECT& rect, const RECT& origin) {
    return RECT{rect.left - origin.left, rect.top - origin.top, rect.right - origin.left,
                rect.bottom - origin.top};
}

RECT SizeLabelRect(const RECT& selection, const RECT& monitor, float scale) {
    const int padding = theme::Scaled(6.0f, scale);
    const int label_width = theme::Scaled(104.0f, scale);
    const int label_height = theme::Scaled(24.0f, scale);
    LONG top = selection.top - label_height - padding - theme::Scaled(theme::kSelectionBorder, scale);
    if (top < monitor.top) { top = selection.top + padding; }
    const LONG left = std::clamp(selection.left, monitor.left + padding,
        std::max(monitor.left + padding, monitor.right - label_width - padding));
    return RECT{left, top, left + label_width, top + label_height};
}

}  // namespace

void RegionSelector::Reset(const RECT& virtual_bounds) {
    bounds_ = virtual_bounds;
    selection_ = RECT{0, 0, 0, 0};
    has_selection_ = false;
    implicit_selection_ = false;
    mode_ = Interaction::Idle;
    active_handle_ = SelectionHandle::None;
    enabled_ = false;
}

void RegionSelector::SetSelection(RECT region) {
    region = platform::NormalizedRect(POINT{region.left, region.top}, POINT{region.right, region.bottom});
    selection_ = ClampToBounds(region);
    has_selection_ = platform::RectWidth(selection_) >= kMinimumSize &&
                     platform::RectHeight(selection_) >= kMinimumSize;
    implicit_selection_ = false;
}

void RegionSelector::SetImplicitSelection(RECT monitor_bounds) {
    monitor_bounds = platform::NormalizedRect(POINT{monitor_bounds.left, monitor_bounds.top},
                                              POINT{monitor_bounds.right, monitor_bounds.bottom});
    selection_ = ClampToBounds(monitor_bounds);
    has_selection_ = platform::RectWidth(selection_) >= kMinimumSize &&
                     platform::RectHeight(selection_) >= kMinimumSize;
    implicit_selection_ = has_selection_;
    mode_ = Interaction::Idle;
    active_handle_ = SelectionHandle::None;
}

RECT RegionSelector::ClampToBounds(RECT region) const {
    region.left = std::clamp(region.left, bounds_.left, bounds_.right);
    region.right = std::clamp(region.right, bounds_.left, bounds_.right);
    region.top = std::clamp(region.top, bounds_.top, bounds_.bottom);
    region.bottom = std::clamp(region.bottom, bounds_.top, bounds_.bottom);
    return region;
}

void RegionSelector::OnMouseDown(POINT pt) {
    if (!enabled_) {
        return;
    }
    cursor_ = pt;
    drag_origin_ = pt;
    drag_origin_selection_ = selection_;

    if (implicit_selection_) {
        // Preserve the full-monitor proposal until movement becomes a usable
        // custom rectangle. This keeps a click (and the first click in a
        // double-click) from destroying the default.
        mode_ = Interaction::Creating;
        active_handle_ = SelectionHandle::BottomRight;
        return;
    }

    const SelectionHandle handle = HitTest(pt);
    if (has_selection_ && handle != SelectionHandle::None && handle != SelectionHandle::Body) {
        mode_ = Interaction::Resizing;
        active_handle_ = handle;
        return;
    }
    if (has_selection_ && handle == SelectionHandle::Body) {
        mode_ = Interaction::Moving;
        active_handle_ = SelectionHandle::Body;
        return;
    }

    // Start a brand new selection.
    mode_ = Interaction::Creating;
    active_handle_ = SelectionHandle::BottomRight;
    selection_ = RECT{pt.x, pt.y, pt.x, pt.y};
    has_selection_ = false;
}

void RegionSelector::OnMouseMove(POINT pt, bool button_down) {
    cursor_ = pt;
    if (!enabled_ || !button_down || mode_ == Interaction::Idle) {
        return;
    }

    switch (mode_) {
        case Interaction::Creating: {
            const RECT created = ClampToBounds(platform::NormalizedRect(drag_origin_, pt));
            const bool meaningful = platform::RectWidth(created) >= kMinimumSize &&
                                    platform::RectHeight(created) >= kMinimumSize;
            if (!implicit_selection_ || meaningful) {
                selection_ = created;
                has_selection_ = meaningful;
                implicit_selection_ = false;
            }
            break;
        }
        case Interaction::Moving: {
            const LONG dx = pt.x - drag_origin_.x;
            const LONG dy = pt.y - drag_origin_.y;
            RECT moved = drag_origin_selection_;
            ::OffsetRect(&moved, dx, dy);
            selection_ = platform::ClampRectToBounds(moved, bounds_);
            break;
        }
        case Interaction::Resizing: {
            RECT resized = drag_origin_selection_;
            switch (active_handle_) {
                case SelectionHandle::TopLeft:
                    resized.left = pt.x;
                    resized.top = pt.y;
                    break;
                case SelectionHandle::Top:
                    resized.top = pt.y;
                    break;
                case SelectionHandle::TopRight:
                    resized.right = pt.x;
                    resized.top = pt.y;
                    break;
                case SelectionHandle::Right:
                    resized.right = pt.x;
                    break;
                case SelectionHandle::BottomRight:
                    resized.right = pt.x;
                    resized.bottom = pt.y;
                    break;
                case SelectionHandle::Bottom:
                    resized.bottom = pt.y;
                    break;
                case SelectionHandle::BottomLeft:
                    resized.left = pt.x;
                    resized.bottom = pt.y;
                    break;
                case SelectionHandle::Left:
                    resized.left = pt.x;
                    break;
                default:
                    break;
            }
            selection_ = ClampToBounds(
                platform::NormalizedRect(POINT{resized.left, resized.top}, POINT{resized.right, resized.bottom}));
            has_selection_ = platform::RectWidth(selection_) >= kMinimumSize &&
                             platform::RectHeight(selection_) >= kMinimumSize;
            break;
        }
        case Interaction::Idle:
            break;
    }
}

void RegionSelector::OnMouseUp(POINT pt) {
    if (!enabled_) {
        return;
    }
    OnMouseMove(pt, true);
    mode_ = Interaction::Idle;
    active_handle_ = SelectionHandle::None;

    if (!has_selection_) {
        // A click without a drag simply clears any half-finished rectangle.
        selection_ = RECT{0, 0, 0, 0};
    }
    // Releasing the mouse never confirms: the user adjusts the handles first and
    // then commits explicitly with Enter or a double-click inside the selection.
}

void RegionSelector::OnNudge(int dx, int dy, bool resize) {
    if (!enabled_ || !has_selection_ || implicit_selection_) {
        return;
    }
    RECT updated = selection_;
    if (resize) {
        updated.right += dx;
        updated.bottom += dy;
        updated.right = std::max(updated.right, updated.left + kMinimumSize);
        updated.bottom = std::max(updated.bottom, updated.top + kMinimumSize);
        selection_ = ClampToBounds(updated);
    } else {
        ::OffsetRect(&updated, dx, dy);
        selection_ = platform::ClampRectToBounds(updated, bounds_);
    }
}

SelectionHandle RegionSelector::HitTest(POINT pt) const {
    if (!has_selection_ || implicit_selection_) {
        return SelectionHandle::None;
    }
    const LONG radius = static_cast<LONG>(theme::kHandleHitRadius);
    for (const HandleLayout& layout : kHandles) {
        const POINT center = HandleCenter(selection_, layout);
        if (std::abs(pt.x - center.x) <= radius && std::abs(pt.y - center.y) <= radius) {
            return layout.id;
        }
    }
    if (::PtInRect(&selection_, pt) != FALSE) {
        return SelectionHandle::Body;
    }
    return SelectionHandle::None;
}

bool RegionSelector::CanCommitFromDoubleClick(POINT pt) const {
    return CanCommit() && (implicit_selection_ || HitTest(pt) == SelectionHandle::Body);
}

LPCWSTR RegionSelector::CursorForPoint(POINT pt) const {
    if (!enabled_) {
        return IDC_ARROW;
    }
    switch (HitTest(pt)) {
        case SelectionHandle::TopLeft:
        case SelectionHandle::BottomRight:
            return IDC_SIZENWSE;
        case SelectionHandle::TopRight:
        case SelectionHandle::BottomLeft:
            return IDC_SIZENESW;
        case SelectionHandle::Top:
        case SelectionHandle::Bottom:
            return IDC_SIZENS;
        case SelectionHandle::Left:
        case SelectionHandle::Right:
            return IDC_SIZEWE;
        case SelectionHandle::Body:
            return IDC_SIZEALL;
        case SelectionHandle::None:
        default:
            return IDC_CROSS;
    }
}

void RegionSelector::RenderGdi(HDC dc, const RECT& monitor_rect, float scale, HFONT label_font,
                               bool show_handles, COLORREF accent) const {
    if (dc == nullptr) {
        return;
    }
    const RECT monitor = ToClientRect(monitor_rect, monitor_rect);

    if (!has_selection_) {
        if (enabled_ && ::PtInRect(&monitor_rect, cursor_) != FALSE) {
            // Crosshair guides across the monitor under the pointer.
            const LONG x = cursor_.x - monitor_rect.left;
            const LONG y = cursor_.y - monitor_rect.top;
            const int width = theme::Scaled(1.0f, scale);
            theme::DrawSolidLine(dc, POINT{monitor.left, y}, POINT{monitor.right, y}, theme::Current().guide, width);
            theme::DrawSolidLine(dc, POINT{x, monitor.top}, POINT{x, monitor.bottom}, theme::Current().guide, width);
        }
        return;
    }

    const RECT selection = ToClientRect(selection_, monitor_rect);
    const int border = theme::Scaled(theme::kSelectionBorder, scale);
    RECT frame = selection;
    // Draw inside the selected bounds so whole-monitor proposals have a
    // visible border at the physical screen edge as well.
    theme::FrameSolid(dc, frame, accent, border);

    // Only user-created selections are adjustable. The implicit monitor
    // proposal deliberately has no Body or handles and keeps the crosshair.
    if (show_handles && !implicit_selection_) {
        const int handle = theme::Scaled(theme::kHandleSize, scale);
        const int half = std::max(1, handle / 2);
        for (const HandleLayout& layout : kHandles) {
            const POINT center = HandleCenter(selection_, layout);
            const RECT box{center.x - monitor_rect.left - half, center.y - monitor_rect.top - half,
                           center.x - monitor_rect.left + half, center.y - monitor_rect.top + half};
            if (box.right < monitor.left || box.left > monitor.right || box.bottom < monitor.top ||
                box.top > monitor.bottom) {
                continue;
            }
            theme::FillSolid(dc, box, theme::Current().handle);
            theme::FrameSolid(dc, box, accent, theme::Scaled(1.0f, scale));
        }
    }

    // Size readout, kept inside the monitor.
    wchar_t label[64]{};
    ::swprintf(label, ARRAYSIZE(label), L"%ld \u00D7 %ld", platform::RectWidth(selection_),
               platform::RectHeight(selection_));
    const RECT box = SizeLabelRect(selection, monitor, scale);
    theme::FillRounded(dc, box, theme::Scaled(4.0f, scale), theme::Current().panel);
    theme::DrawLabel(dc, box, label, theme::Current().text, label_font,
                     DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
}

std::array<RECT, 5> RegionSelector::GdiSelectionDamage(
    const RECT& selection, const RECT& monitor_rect, float scale) {
    const RECT rect = ToClientRect(selection, monitor_rect);
    // Include handles even when hidden, allowing implicit -> adjustable changes
    // and very small selections without a separate invalidation policy.
    const LONG margin = std::max(theme::Scaled(theme::kHandleSize, scale) / 2,
                                 theme::Scaled(theme::kSelectionBorder, scale)) + 1;
    const RECT monitor = ToClientRect(monitor_rect, monitor_rect);
    std::array<RECT, 5> damage{{
        {rect.left - margin, rect.top - margin, rect.right + margin, rect.top + margin},
        {rect.left - margin, rect.bottom - margin, rect.right + margin, rect.bottom + margin},
        {rect.left - margin, rect.top - margin, rect.left + margin, rect.bottom + margin},
        {rect.right - margin, rect.top - margin, rect.right + margin, rect.bottom + margin},
        SizeLabelRect(rect, monitor, scale)
    }};
    // The antialiased rounded label has one physical pixel of edge coverage
    // beyond its nominal right/bottom coordinates, independent of UI scale.
    ::InflateRect(&damage.back(),1,1);
    for (RECT& part : damage) { part = platform::IntersectRects(part, monitor); }
    return damage;
}

}  // namespace easnap::ui
