// Capture-workspace interaction policy; independent of recording lifecycle.
#pragma once
#include "app/session_state.h"
#include "ui/ui_theme.h"

namespace easnap::ui {
enum class WorkspaceInteraction { Standby, Editing };

class CaptureWorkspace {
public:
    void Reset() noexcept { *this = {}; }
    void EnterEditing() noexcept { interaction_ = WorkspaceInteraction::Editing; }
    void SetMode(app::CaptureMode mode) noexcept;
    void EditorFeatureUsed() noexcept;
    void FixScreenshot() noexcept { if (screenshot_visible()) { screenshot_fixed_ = true; } }
    [[nodiscard]] bool screenshot_fixed() const noexcept { return screenshot_fixed_; }
    [[nodiscard]] bool can_adjust_selection() const noexcept { return !screenshot_fixed_; }
    [[nodiscard]] bool can_switch_mode() const noexcept { return !screenshot_fixed_; }
    [[nodiscard]] bool ConsumeQuickReturn() noexcept;
    // Called only after an actual successful document revision/gesture commit.
    // Consumes the automatic attempt before publication, including failures.
    [[nodiscard]] bool DocumentCommitted() noexcept;
    [[nodiscard]] WorkspaceInteraction interaction() const noexcept { return interaction_; }
    [[nodiscard]] app::CaptureMode mode() const noexcept { return mode_; }
    [[nodiscard]] bool editing() const noexcept { return interaction_ == WorkspaceInteraction::Editing; }
    [[nodiscard]] bool magnifier_visible() const noexcept { return !editing(); }
    [[nodiscard]] bool screenshot_visible() const noexcept { return editing() && mode_ == app::CaptureMode::Screenshot; }
    [[nodiscard]] bool recording_visible() const noexcept { return editing() && mode_ == app::CaptureMode::Record; }
    [[nodiscard]] bool quick_return_armed() const noexcept { return quick_return_armed_; }
    [[nodiscard]] bool automatic_attempt_consumed() const noexcept { return automatic_attempt_consumed_; }
    [[nodiscard]] COLORREF accent() const noexcept {
        return mode_ == app::CaptureMode::Screenshot ? theme::Current().screenshot_border : theme::Current().record_border;
    }
private:
    WorkspaceInteraction interaction_{WorkspaceInteraction::Standby};
    app::CaptureMode mode_{app::CaptureMode::Screenshot};
    bool editor_feature_used_{false};
    bool quick_return_armed_{false};
    bool automatic_attempt_consumed_{false};
    bool screenshot_fixed_{false};
};

enum class WorkspacePointerTarget { Content, Creation, Handle, Annotation, Excluded };
struct WorkspacePointerMetrics {
    LONG drag_width{}, drag_height{}, double_width{}, double_height{};
    DWORD double_time{};
    [[nodiscard]] static WorkspacePointerMetrics Native() noexcept;
};

// Windows supplies the second-down classification. Completion waits only for
// that gesture's release, so a second-click drag can still cancel quick-copy.
class WorkspacePointerSequence {
public:
    void Down(POINT point, DWORD time, bool native_double_click, WorkspacePointerTarget target,
              WorkspacePointerMetrics metrics) noexcept;
    void Move(POINT point) noexcept;
    [[nodiscard]] bool Up(POINT point) noexcept;
    void Cancel() noexcept { *this = {}; }
    [[nodiscard]] bool dragged() const noexcept { return dragged_; }
    [[nodiscard]] POINT origin() const noexcept { return origin_; }
    [[nodiscard]] WorkspacePointerTarget target() const noexcept { return target_; }
private:
    WorkspacePointerMetrics metrics_{};
    WorkspacePointerTarget target_{WorkspacePointerTarget::Excluded};
    WorkspacePointerTarget previous_target_{WorkspacePointerTarget::Excluded};
    POINT origin_{}, previous_origin_{};
    DWORD down_time_{}, previous_time_{};
    bool active_{false}, dragged_{false}, second_click_{false}, previous_click_{false};
};

inline constexpr uint64_t kModeToolbarFadeDurationMs = 500;
/// Smoothstep opacity for the native mode-toolbar fade; only the small toolbar
/// changes opacity, never the selection/image/dimming surfaces.
[[nodiscard]] BYTE ModeToolbarOpacity(uint64_t elapsed_ms) noexcept;

// Brief, non-activating green notice with a lifetime independent of the workspace.
void ShowCaptureCopiedNotice(const RECT& region);
void ShowCaptureStatusNotice(const RECT& region, const std::wstring& text);
} // namespace easnap::ui
