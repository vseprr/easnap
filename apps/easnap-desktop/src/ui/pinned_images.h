// Easnap - screenshots pinned to the screen (ADR-038).
#pragma once

#include "platform/platform_utils.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace easnap::app {
struct AppSettings;
} // namespace easnap::app

namespace easnap::ui {

// Commands of a pin's menu. A pin window also accepts them as WM_COMMAND.
inline constexpr UINT kPinCommandCopy = 0xE501;
inline constexpr UINT kPinCommandSave = 0xE502;
inline constexpr UINT kPinCommandOriginalSize = 0xE503;
inline constexpr UINT kPinCommandClose = 0xE504;
inline constexpr UINT kPinCommandCloseAll = 0xE505;

/// Finished screenshots kept on the desktop as small topmost windows. They
/// outlive the capture session that made them and are screen content: later
/// screenshots, Scrollshots and recordings contain them.
class PinnedImages final {
public:
    /// Save reads and updates `settings`, which must outlive the pins. Without
    /// settings the Save dialog opens in its own folder.
    explicit PinnedImages(app::AppSettings* settings);
    ~PinnedImages();
    PinnedImages(const PinnedImages&) = delete;
    PinnedImages& operator=(const PinnedImages&) = delete;

    /// Shows `image` as a new pin over `source`, in desktop pixels, and
    /// activates it. False when its window could not be created.
    [[nodiscard]] bool Pin(platform::BgraImage image, const RECT& source);
    /// Closes every pin. A pin whose menu or Save dialog is open closes when
    /// that returns.
    void CloseAll();
    /// The windows of the open pins, oldest first.
    [[nodiscard]] std::vector<HWND> windows() const;
    [[nodiscard]] size_t count() const noexcept;

private:
    class Window;
    app::AppSettings* settings_{nullptr};
    /// A closed pin has released its window and image; its object is removed
    /// by the next Pin, never while one of its own messages is being handled.
    std::vector<std::unique_ptr<Window>> pins_;
};

} // namespace easnap::ui
