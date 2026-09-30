// Easnap - user-facing recording configuration, boundary and live controls.
#pragma once

#include <functional>
#include <memory>

#include "app/recording_workflow.h"
#include "app/appearance_theme.h"

namespace easnap::ui {

/// Owns the small recording panel and four click-through boundary bars. The
/// coordinator remains the sole SessionState owner; this class only reflects a
/// supplied state and forwards user commands.
class RecordingControls final {
public:
    struct Callbacks {
        std::function<void(app::RecordingConfiguration)> on_start;
        std::function<void()> on_cancel;
        std::function<void()> on_pause;
        std::function<void()> on_resume;
        std::function<void()> on_stop;
        std::function<void()> on_status_tick;
        std::function<void(app::AppearanceTheme)> on_appearance;
    };

    explicit RecordingControls(Callbacks callbacks);
    ~RecordingControls();

    RecordingControls(const RecordingControls&) = delete;
    RecordingControls& operator=(const RecordingControls&) = delete;

    [[nodiscard]] bool ShowConfiguring(
        const RECT& region, app::RecordingConfiguration configuration, bool workspace_hosted = false,
        HWND workspace_owner = nullptr);
    void SetRegion(const RECT& region);
    void RetireWorkspace();
    [[nodiscard]] app::RecordingConfiguration configuration() const;
    void SetSessionState(app::SessionState state);
    void SetElapsed(uint64_t active_duration_100ns);
    void SetError(const std::wstring& message);
    void Activate();
    void RefreshLayout();
    void Hide();
    void Destroy();

    /// Provides dialog-style Tab/Enter/Esc navigation for the native controls.
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] HWND hwnd() const noexcept;

private:
    class Window;

    Callbacks callbacks_;
    std::unique_ptr<Window> window_;
    RECT region_{};
    app::SessionState state_{app::SessionState::RecordingConfiguring};
    app::RecordingConfiguration configuration_{};
    bool visible_{false};
    bool workspace_hosted_{false};
};

}  // namespace easnap::ui
