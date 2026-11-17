#pragma once

#include <windows.h>
// windows.h comes before shellapi.h, which depends on its types.
#include <shellapi.h>

#include <string>

namespace easnap::ui {

/// Owns the notification-area icon for the lifetime of the process.
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon() { Remove(); }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    /// Adds the icon for `owner`, which receives kMsgTrayIcon; false when the
    /// shell refuses it.
    bool Add(HWND owner, const std::wstring& tooltip);
    /// Takes the icon out of the notification area and releases its image.
    void Remove();
    /// Re-adds the icon after an Explorer restart (WM_TASKBARCREATED).
    void Restore();
    /// Changes the tooltip; kept for the next Add when there is no icon.
    void SetTooltip(const std::wstring& tooltip);
    /// True when the shell took the notification; false without an icon.
    bool ShowBalloon(const wchar_t* title, const wchar_t* text) const;

    /// True while the icon is in the notification area.
    [[nodiscard]] bool added() const noexcept { return added_; }

private:
    [[nodiscard]] NOTIFYICONDATAW MakeData() const;

    HWND owner_{nullptr};
    HICON icon_{nullptr};
    std::wstring tooltip_;
    bool added_{false};
};

}  // namespace easnap::ui
