#include "ui/tray_icon.h"

#include "platform/app_messages.h"
#include "platform/platform_utils.h"

namespace easnap::ui {
namespace {

constexpr UINT kTrayIconId = 1;

}  // namespace

bool TrayIcon::Add(HWND owner, const std::wstring& tooltip) {
    owner_ = owner;
    tooltip_ = tooltip;
    // Added again after an Explorer restart: the icon is loaded once.
    if (icon_ == nullptr) { icon_ = easnap::platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON)); }

    NOTIFYICONDATAW data = MakeData();
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = easnap::app::kMsgTrayIcon;
    data.hIcon = icon_;
    ::wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);

    if (::Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        easnap::platform::LogMessage(L"Shell_NotifyIcon(NIM_ADD) failed (error %lu)", ::GetLastError());
        return false;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    if (::Shell_NotifyIconW(NIM_SETVERSION, &data) == FALSE) {
        // The icon keeps the old callbacks: clicks work, WM_CONTEXTMENU does not come.
        easnap::platform::LogMessage(L"Shell_NotifyIcon(NIM_SETVERSION) failed (error %lu)", ::GetLastError());
    }
    added_ = true;
    return true;
}

void TrayIcon::Remove() {
    if (added_) {
        NOTIFYICONDATAW data = MakeData();
        ::Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }
    if (icon_ != nullptr) {
        ::DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void TrayIcon::Restore() {
    added_ = false;
    if (owner_ != nullptr) {
        Add(owner_, tooltip_);
    }
}

void TrayIcon::SetTooltip(const std::wstring& tooltip) {
    tooltip_ = tooltip;
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW data = MakeData();
    data.uFlags = NIF_TIP | NIF_SHOWTIP;
    ::wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
    if (::Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        easnap::platform::LogMessage(L"Shell_NotifyIcon(NIM_MODIFY) for the tooltip failed (error %lu)",
                                     ::GetLastError());
    }
}

bool TrayIcon::ShowBalloon(const wchar_t* title, const wchar_t* text) const {
    if (!added_) {
        return false;
    }
    NOTIFYICONDATAW data = MakeData();
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    ::wcsncpy_s(data.szInfoTitle, title, _TRUNCATE);
    ::wcsncpy_s(data.szInfo, text, _TRUNCATE);
    if (::Shell_NotifyIconW(NIM_MODIFY, &data) == FALSE) {
        easnap::platform::LogMessage(L"Shell_NotifyIcon(NIM_MODIFY) for a notification failed (error %lu)",
                                     ::GetLastError());
        return false;
    }
    return true;
}

NOTIFYICONDATAW TrayIcon::MakeData() const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner_;
    data.uID = kTrayIconId;
    return data;
}

}  // namespace easnap::ui
