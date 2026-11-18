#include "ui/about_dialog.h"

#include <cmath>
#include <cstring>

// windows.h, from the header, comes before shellapi.h.
#include <shellapi.h>

#include "platform/screen_geometry.h"
#include "platform/system_info.h"
#include "resource.h"
#include "strings/strings.h"
#include "ui/message_dialog.h"
#include "ui/native_controls.h"

namespace easnap::ui {
namespace {

constexpr wchar_t kWebsite[] = L"https://github.com/vseprr/easnap";

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (::OpenClipboard(owner) == FALSE) {
        return false;
    }
    bool copied = false;
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = ::GlobalLock(memory)) {
            ::memcpy(target, text.c_str(), bytes);
            ::GlobalUnlock(memory);
            copied = ::EmptyClipboard() != FALSE && ::SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
        }
        if (!copied) { ::GlobalFree(memory); }
    }
    ::CloseClipboard();
    return copied;
}

/// What a bug report needs, as plain text: versions and the display layout.
/// No path, no file name, nothing the user created.
std::wstring DiagnosticDetails(const AboutShortcut& shortcut) {
    const easnap::platform::WindowsVersion windows = easnap::platform::CurrentWindowsVersion();
    std::wstring text = std::wstring(L"Easnap ") + EASNAP_VERSION_TEXT_W + L"\r\n";
    text += L"Windows " + std::to_wstring(windows.major) + L"." + std::to_wstring(windows.minor) + L" build " +
            std::to_wstring(windows.build) + L"\r\n";
    text += std::wstring(L"Interface language: ") +
            (easnap::strings::Current() == easnap::strings::Language::Turkish ? L"Turkish" : L"English") + L"\r\n";
    text += std::wstring(L"Capture shortcut: ") + shortcut.display_text +
            (shortcut.registered ? L"" : L" (not available)") + L"\r\n";
    text += std::wstring(L"Scrollshot available: ") + (easnap::ui::CaptureExclusionSupported() ? L"yes" : L"no") + L"\r\n";
    const auto monitors = easnap::platform::EnumerateMonitors();
    text += L"Displays: " + std::to_wstring(monitors.size()) + L"\r\n";
    for (const auto& monitor : monitors) {
        text += L"  " + std::to_wstring(easnap::platform::RectWidth(monitor.bounds)) + L" x " +
                std::to_wstring(easnap::platform::RectHeight(monitor.bounds)) + L" at " +
                std::to_wstring(monitor.bounds.left) + L"," + std::to_wstring(monitor.bounds.top) + L", " +
                std::to_wstring(std::lround(monitor.scale() * 100.0f)) + L"%" +
                (monitor.primary ? L", primary" : L"") + L"\r\n";
    }
    return text;
}

}  // namespace

/// About: version, shortcut and Windows build, with Copy details and Website.
/// The shortcut is asked for again when the details are copied.
void ShowAboutDialog(HWND owner, const std::function<AboutShortcut()>& shortcut) {
    namespace strings = easnap::strings;
    const easnap::platform::WindowsVersion windows = easnap::platform::CurrentWindowsVersion();
    const std::wstring text = strings::Format(
        IDS_ABOUT_TEXT, {EASNAP_VERSION_TEXT_W, shortcut().display_text,
                         std::to_wstring(windows.build)});
    enum : int { kCopy = 100,
                 kWeb = 101,
                 kClose = 102 };
    const int choice = easnap::ui::ShowChoice(
        nullptr, text.c_str(), strings::Get(IDS_SHELL_ABOUT_TITLE),
        {{kCopy, strings::Get(IDS_ABOUT_COPY_DETAILS)}, {kWeb, strings::Get(IDS_ABOUT_WEBSITE)}, {kClose, strings::Get(IDS_SHELL_CHROME_CLOSE)}},
        kClose, kClose);
    if (choice == kCopy) {
        CopyTextToClipboard(owner, DiagnosticDetails(shortcut()));
    } else if (choice == kWeb) {
        ::ShellExecuteW(nullptr, L"open", kWebsite, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

}  // namespace easnap::ui
