// Easnap - MP4 preview and bounded trim controls.
#pragma once

#include "ui/trim_layout.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "app/trim_workflow.h"
#include "media/media_preview.h"
#include "ui/timeline_accessibility.h"

namespace easnap::ui {

/// User-facing trim surface. It owns preview/UI resources only; the coordinator
/// owns artifacts, SessionState, and background export lifetime.
class TrimWindow final {
public:
    struct Callbacks final {
        std::function<void(media::MediaTime, media::MediaTime)> on_save;
        std::function<void(media::MediaTime, media::MediaTime)> on_copy;
        std::function<void()> on_cancel_export;
        std::function<void()> on_discard_requested;
        std::function<void()> on_status_tick;
    };

    explicit TrimWindow(Callbacks callbacks);
    ~TrimWindow();

    TrimWindow(const TrimWindow&) = delete;
    TrimWindow& operator=(const TrimWindow&) = delete;

    [[nodiscard]] bool Open(const std::wstring& source_path,
                            const RECT& source_region);
    void SetExporting(bool exporting, bool cancelling = false);
    void SetExportProgress(float progress);
    /// Returns to editing with one coherent layout after a retained-window result.
    void SetExportResult(const std::wstring& message, bool error = false);
    void SetStatus(const std::wstring& message, bool error = false);
    void Activate();
    void RefreshLayout();
    /// Releases preview/media handles before the coordinator deletes a session.
    void ReleaseMedia();
    void Hide();
    void Destroy();

    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] bool exporting() const noexcept { return exporting_; }
    /// True when there is no working preview. Nothing can be trimmed then and
    /// Save is off; the coordinator offers to save the recording as it is.
    [[nodiscard]] bool preview_failed() const noexcept;
    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }
    [[nodiscard]] const app::TrimRangeModel& range() const noexcept {
        return range_;
    }

private:
    enum class TimelineDrag { None, Seek, In, Out };

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam);
    static LRESULT CALLBACK TimelineProc(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam);
    static LRESULT CALLBACK VideoProc(HWND hwnd, UINT message, WPARAM wparam,
                                      LPARAM lparam);
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT HandleTimelineMessage(HWND hwnd, UINT message, WPARAM wparam,
                                  LPARAM lparam);
    LRESULT HandleVideoMessage(HWND hwnd, UINT message, WPARAM wparam,
                               LPARAM lparam);

    [[nodiscard]] bool EnsureWindowClasses();
    [[nodiscard]] bool CreateControls();
    [[nodiscard]] HWND CreateControl(const wchar_t* class_name,
                                     const wchar_t* text, DWORD style,
                                     int identifier) const;
    void LayoutControls(int client_width, int client_height);
    void UpdateStatusLayout();
    void RebuildFonts();
    void UpdatePreviewState();
    void UpdatePosition();
    void UpdateLabels();
    void UpdateEnabledState();
    void HandleCommand(int identifier);
    void TogglePlayback();
    void SeekTo(media::MediaTime position);
    void ApplyPlaybackDecision(const app::TrimPlaybackDecision& decision);
    void ReconcileRangeEdit();
    void SetInFromCurrent();
    void SetOutFromCurrent();
    void BeginSave();
    void BeginCopy();
    void RequestClose();
    void PaintTimeline(HWND hwnd, HDC print_dc = nullptr);
    [[nodiscard]] AccessibleTimelineSnapshot AccessibleTimeline() const;
    bool SetAccessibleTimelineValue(int index, double seconds);
    void FocusTimeline(int index);
    void UpdateTimelineFromPoint(int x, TimelineDrag drag);
    [[nodiscard]] int TimelineTrackLeft(HWND hwnd) const noexcept;
    [[nodiscard]] int TimelineTrackWidth(HWND hwnd) const noexcept;

    Callbacks callbacks_;
    std::unique_ptr<media::IMediaPreview> preview_;
    app::TrimRangeModel range_;
    RECT source_region_{};
    HWND hwnd_{nullptr};
    HWND video_{nullptr};
    HWND time_label_{nullptr};
    HWND range_label_{nullptr};
    HWND status_{nullptr};
    HWND help_{nullptr};
    HWND timeline_{nullptr};
    std::unique_ptr<TimelineAccessibility> timeline_accessibility_;
    int timeline_focus_{1};
    HWND play_pause_{nullptr};
    HWND set_in_{nullptr};
    HWND set_out_{nullptr};
    HWND save_{nullptr};
    HWND copy_{nullptr};
    HWND cancel_export_{nullptr};
    HWND discard_{nullptr};
    HWND progress_{nullptr};
    HFONT ui_font_{nullptr};
    std::wstring status_message_;
    std::wstring time_text_;
    std::wstring range_text_;
    TrimWindowLayout layout_{};
    float scale_{1.0f};
    TimelineDrag timeline_drag_{TimelineDrag::None};
    std::optional<media::MediaTime> timeline_seek_position_;
    bool preview_error_reported_{false};
    bool status_error_{false};
    bool exporting_{false};
    bool cancelling_{false};
    ULONGLONG export_indicator_due_ms_{0};
    bool visible_{false};
    bool closing_{false};
};

}  // namespace easnap::ui
