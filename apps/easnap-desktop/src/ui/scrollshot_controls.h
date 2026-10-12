// Easnap - Scrollshot island (Start/help, then the four actions) and live preview.
//
// Presentation only. The coordinator owns the session state and the capture;
// this class shows what it is told and forwards commands.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "platform/platform_utils.h"
#include "scrollshot/scrollshot_session.h"

namespace easnap::ui {

class ScrollshotControls final {
public:
    enum class Action { Start, Help, Edit, Save, Discard, Copy };
    /// `Start` offers Start Scrollshot and help beside an adjustable frame.
    /// `Capture` offers exactly Edit, Save, Discard and Copy, with a status.
    enum class View { Start, Capture };

    struct Callbacks {
        std::function<void(Action)> on_action;
        /// A wheel turn over the frame reached the island instead of the content.
        std::function<void()> on_wheel_blocked;
    };

    explicit ScrollshotControls(Callbacks callbacks);
    ~ScrollshotControls();

    ScrollshotControls(const ScrollshotControls&) = delete;
    ScrollshotControls& operator=(const ScrollshotControls&) = delete;

    /// Shows the island for `view` next to `frame`. `owner` keeps it above the
    /// capture workspace.
    [[nodiscard]] bool Show(View view, const RECT& frame, HWND owner);
    /// The frame moved or the displays changed: places the surfaces again.
    void SetFrame(const RECT& frame);
    void SetStatus(const std::wstring& size, const std::wstring& message, bool warning);
    void SetPreview(const scrollshot::ScrollshotPreview& preview);
    /// Disables the actions while one of them is being carried out.
    void SetBusy(bool busy);
    void Activate();
    void Hide();
    void Destroy();

    /// Dialog-style Tab/Enter/Esc navigation plus Ctrl+S and Ctrl+C.
    [[nodiscard]] bool ProcessDialogMessage(MSG& message) const;

    [[nodiscard]] bool visible() const noexcept { return visible_; }
    [[nodiscard]] View view() const noexcept { return view_; }
    [[nodiscard]] HWND hwnd() const noexcept;
    [[nodiscard]] HWND preview_hwnd() const noexcept;
    [[nodiscard]] const RECT& frame() const noexcept { return frame_; }
    /// True when no monitor had room for the surface outside the frame.
    [[nodiscard]] bool island_over_frame() const noexcept { return island_over_frame_; }
    [[nodiscard]] bool preview_over_frame() const noexcept { return preview_over_frame_; }

    /// Width in pixels of the reduced image the preview shows for `frame`.
    [[nodiscard]] static int PreviewPixelWidth(const RECT& frame);

private:
    class Window;

    void Dispatch(Action action) const { if (callbacks_.on_action) { callbacks_.on_action(action); } }

    Callbacks callbacks_;
    std::unique_ptr<Window> window_;
    RECT frame_{};
    View view_{View::Start};
    bool visible_{false};
    bool island_over_frame_{false};
    bool preview_over_frame_{false};
};

}  // namespace easnap::ui
