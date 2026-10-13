#include "ui/native_controls.h"
// Easnap - application entry point.
//
// Easnap is a tray-resident tool: there is no main window. A hidden message-only
// style window owns the notification icon, the global shortcut and the single
// message loop that every UI surface (overlay, editor) is pumped from.
//
// Design rules enforced here:
//   * Exactly one instance may run, so the global shortcut has a single owner.
//   * While idle the process holds no capture, encoder or renderer resources.
//   * Windows are always destroyed outside their own window procedure, via the
//     deferred kMsgSessionCleanup message.
#include <windows.h>
// windows.h must stay first: the shell headers below depend on its types.
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "app/session_state.h"
#include "platform/diagnostics.h"
#include "platform/platform_utils.h"
#include "platform/startup_entry.h"
#include "resource.h"
#include "strings/strings.h"
#include "ui/settings_dialog.h"

// Opt into the version 6 common controls so menus and dialogs use the modern
// visual style. Doing it from the linker keeps the project free of a hand
// written manifest that would otherwise clash with the CMake generated one.
#if defined(_MSC_VER)
#pragma comment(linker,                                        \
                "/manifestdependency:\"type='win32' "          \
                "name='Microsoft.Windows.Common-Controls' "    \
                "version='6.0.0.0' processorArchitecture='*' " \
                "publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace {

using easnap::app::CaptureCoordinator;
using easnap::app::SessionState;

constexpr wchar_t kHostClassName[] = L"EasnapHostWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Easnap.SingleInstance";
constexpr wchar_t kProductName[] = L"Easnap";
constexpr UINT kMsgTrayIcon = WM_APP + 1;
constexpr UINT kTrayIconId = 1;

/// Owns the notification-area icon for the lifetime of the process.
class TrayIcon {
public:
    TrayIcon() = default;
    ~TrayIcon() { Remove(); }

    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    bool Add(HWND owner, const std::wstring& tooltip) {
        owner_ = owner;
        tooltip_ = tooltip;
        // Added again after an Explorer restart: the icon is loaded once.
        if (icon_ == nullptr) { icon_ = easnap::platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON)); }

        NOTIFYICONDATAW data = MakeData();
        data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
        data.uCallbackMessage = kMsgTrayIcon;
        data.hIcon = icon_;
        ::wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);

        if (::Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
            easnap::platform::LogMessage(L"Shell_NotifyIcon(NIM_ADD) failed (error %lu)", ::GetLastError());
            return false;
        }
        data.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data);
        added_ = true;
        return true;
    }

    void Remove() {
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

    /// Re-adds the icon after an Explorer restart (WM_TASKBARCREATED).
    void Restore() {
        added_ = false;
        if (owner_ != nullptr) {
            Add(owner_, tooltip_);
        }
    }

    void SetTooltip(const std::wstring& tooltip) {
        tooltip_ = tooltip;
        if (!added_) {
            return;
        }
        NOTIFYICONDATAW data = MakeData();
        data.uFlags = NIF_TIP | NIF_SHOWTIP;
        ::wcsncpy_s(data.szTip, tooltip_.c_str(), _TRUNCATE);
        ::Shell_NotifyIconW(NIM_MODIFY, &data);
    }

    void ShowBalloon(const wchar_t* title, const wchar_t* text) const {
        if (!added_) {
            return;
        }
        NOTIFYICONDATAW data = MakeData();
        data.uFlags = NIF_INFO;
        data.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
        ::wcsncpy_s(data.szInfoTitle, title, _TRUNCATE);
        ::wcsncpy_s(data.szInfo, text, _TRUNCATE);
        ::Shell_NotifyIconW(NIM_MODIFY, &data);
    }

private:
    [[nodiscard]] NOTIFYICONDATAW MakeData() const {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = owner_;
        data.uID = kTrayIconId;
        return data;
    }

    HWND owner_{nullptr};
    HICON icon_{nullptr};
    std::wstring tooltip_;
    bool added_{false};
};

constexpr wchar_t kWebsite[] = L"https://github.com/vseprr/easnap";
constexpr wchar_t kReleasesPage[] = L"https://github.com/vseprr/easnap/releases";
/// A second launch posts this to the running instance: start a capture.
constexpr wchar_t kActivateMessageName[] = L"Easnap.StartCapture";
constexpr int kProbeHotkeyId = 2;

TrayIcon g_tray;
UINT g_taskbar_created_message = 0;
UINT g_activate_message = 0;
bool g_hotkey_registered = false;

/// The tooltip says how to capture: with the shortcut, or by clicking when
/// another application holds the shortcut.
std::wstring TrayTooltip() {
    return easnap::strings::Format(g_hotkey_registered ? IDS_SHELL_TRAY_TOOLTIP : IDS_TRAY_TOOLTIP_NO_SHORTCUT,
                                   {CaptureCoordinator::Instance().settings().HotkeyDisplayText()});
}

bool RegisterCaptureHotkey(HWND hwnd) {
    const auto& settings = CaptureCoordinator::Instance().settings();
    g_hotkey_registered = ::RegisterHotKey(hwnd, IDH_EASNAP_CAPTURE, settings.hotkey_modifiers | MOD_NOREPEAT,
                                           settings.hotkey_virtual_key) != FALSE;
    if (!g_hotkey_registered) {
        easnap::platform::LogMessage(L"RegisterHotKey failed (error %lu)", ::GetLastError());
    }
    return g_hotkey_registered;
}

void UnregisterCaptureHotkey(HWND hwnd) {
    if (g_hotkey_registered) {
        ::UnregisterHotKey(hwnd, IDH_EASNAP_CAPTURE);
        g_hotkey_registered = false;
    }
}

easnap::strings::LanguagePreference ToPreference(easnap::app::LanguageSetting language) {
    switch (language) {
        case easnap::app::LanguageSetting::English: return easnap::strings::LanguagePreference::English;
        case easnap::app::LanguageSetting::Turkish: return easnap::strings::LanguagePreference::Turkish;
        case easnap::app::LanguageSetting::System: break;
    }
    return easnap::strings::LanguagePreference::System;
}

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
std::wstring DiagnosticDetails() {
    const auto& settings = CaptureCoordinator::Instance().settings();
    const easnap::platform::WindowsVersion windows = easnap::platform::CurrentWindowsVersion();
    std::wstring text = std::wstring(L"Easnap ") + EASNAP_VERSION_TEXT_W + L"\r\n";
    text += L"Windows " + std::to_wstring(windows.major) + L"." + std::to_wstring(windows.minor) + L" build " +
            std::to_wstring(windows.build) + L"\r\n";
    text += std::wstring(L"Interface language: ") +
            (easnap::strings::Current() == easnap::strings::Language::Turkish ? L"Turkish" : L"English") + L"\r\n";
    text += std::wstring(L"Capture shortcut: ") + settings.HotkeyDisplayText() +
            (g_hotkey_registered ? L"" : L" (not available)") + L"\r\n";
    text += std::wstring(L"Scrollshot available: ") + (easnap::ui::CaptureExclusionSupported() ? L"yes" : L"no") + L"\r\n";
    const auto monitors = easnap::platform::EnumerateMonitors();
    text += L"Displays: " + std::to_wstring(monitors.size()) + L"\r\n";
    for (const auto& monitor : monitors) {
        text += L"  " + std::to_wstring(easnap::platform::RectWidth(monitor.bounds)) + L" x " +
                std::to_wstring(easnap::platform::RectHeight(monitor.bounds)) + L" at " +
                std::to_wstring(monitor.bounds.left) + L"," + std::to_wstring(monitor.bounds.top) + L", " +
                std::to_wstring(static_cast<int>(monitor.scale() * 100.0f + 0.5f)) + L"%" +
                (monitor.primary ? L", primary" : L"") + L"\r\n";
    }
    return text;
}

void ShowAboutDialog(HWND owner) {
    namespace strings = easnap::strings;
    const easnap::platform::WindowsVersion windows = easnap::platform::CurrentWindowsVersion();
    const std::wstring text = strings::Format(
        IDS_ABOUT_TEXT, {EASNAP_VERSION_TEXT_W, CaptureCoordinator::Instance().settings().HotkeyDisplayText(),
                         std::to_wstring(windows.build)});
    enum : int { kCopy = 100, kWeb = 101, kClose = 102 };
    const int choice = easnap::ui::ShowChoice(
        nullptr, text.c_str(), strings::Get(IDS_SHELL_ABOUT_TITLE),
        {{kCopy, strings::Get(IDS_ABOUT_COPY_DETAILS)}, {kWeb, strings::Get(IDS_ABOUT_WEBSITE)},
         {kClose, strings::Get(IDS_SHELL_CHROME_CLOSE)}},
        kClose, kClose);
    if (choice == kCopy) {
        CopyTextToClipboard(owner, DiagnosticDetails());
    } else if (choice == kWeb) {
        ::ShellExecuteW(nullptr, L"open", kWebsite, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

/// The Settings dialog, and what accepting it changes outside the settings
/// file: the registered shortcut, the startup entry, language and appearance.
void ShowSettings(HWND hwnd) {
    namespace strings = easnap::strings;
    auto& coordinator = CaptureCoordinator::Instance();
    if (coordinator.state() != SessionState::Idle) {
        return;
    }
    auto& settings = coordinator.settings();
    const std::wstring executable = easnap::platform::CurrentExecutablePath();
    easnap::ui::SettingsDialogModel model;
    model.language = settings.language;
    model.appearance_follows_system = settings.appearance_follows_system;
    model.appearance_theme = settings.appearance_theme;
    model.hotkey_modifiers = settings.hotkey_modifiers;
    model.hotkey_virtual_key = settings.hotkey_virtual_key;
    model.start_with_windows = easnap::platform::StartupEntryEnabled(executable);
    model.capture_cursor = settings.capture_cursor;
    model.image_folder = settings.image_folder;
    model.video_folder = settings.video_folder;
    model.quick_save_images = settings.quick_save_images;
    const bool started_with_windows = model.start_with_windows;

    easnap::ui::SettingsDialogCallbacks callbacks;
    // Registering under a second identifier tells whether the combination is
    // free without giving up the shortcut that works now.
    callbacks.shortcut_available = [hwnd](UINT modifiers, UINT key) {
        if (::RegisterHotKey(hwnd, kProbeHotkeyId, modifiers | MOD_NOREPEAT, key) == FALSE) {
            return false;
        }
        ::UnregisterHotKey(hwnd, kProbeHotkeyId);
        return true;
    };
    callbacks.choose_folder = [](HWND owner, const std::wstring& current) {
        return easnap::ui::ChooseFolder(owner, current, easnap::strings::Get(IDS_SETTINGS_FOLDER_PICK_TITLE));
    };
    // The shortcut is given up while the dialog is open. Windows hands a
    // registered combination to its owner and never to the window that wants
    // to read it, and Easnap's own is the one most likely to be pressed here.
    UnregisterCaptureHotkey(hwnd);
    if (!easnap::ui::ShowSettingsDialog(nullptr, model, callbacks) || coordinator.state() != SessionState::Idle) {
        RegisterCaptureHotkey(hwnd);
        g_tray.SetTooltip(TrayTooltip());
        return;
    }

    settings.language = model.language;
    strings::SetPreference(ToPreference(settings.language));
    settings.appearance_follows_system = model.appearance_follows_system;
    settings.appearance_theme = model.appearance_theme;
    coordinator.RefreshAppearance();
    settings.capture_cursor = model.capture_cursor;
    settings.image_folder = model.image_folder;
    settings.video_folder = model.video_folder;
    settings.quick_save_images = model.quick_save_images;
    const UINT previous_modifiers = settings.hotkey_modifiers;
    const UINT previous_key = settings.hotkey_virtual_key;
    settings.hotkey_modifiers = model.hotkey_modifiers;
    settings.hotkey_virtual_key = model.hotkey_virtual_key;
    if (!RegisterCaptureHotkey(hwnd) &&
        (previous_modifiers != settings.hotkey_modifiers || previous_key != settings.hotkey_virtual_key)) {
        // Taken between the check and now: keep what worked before.
        settings.hotkey_modifiers = previous_modifiers;
        settings.hotkey_virtual_key = previous_key;
        RegisterCaptureHotkey(hwnd);
    }
    const bool startup_refused = model.start_with_windows != started_with_windows &&
                                 !easnap::platform::SetStartupEntry(model.start_with_windows, executable);
    coordinator.PersistSettings();
    g_tray.SetTooltip(TrayTooltip());
    easnap::platform::LogMessage(L"Settings changed");
    if (startup_refused) {
        // The next visit to Settings shows the entry as it really is.
        easnap::platform::LogMessage(L"The startup entry could not be changed");
        easnap::ui::ShowErrorMessage(nullptr, strings::Get(IDS_SETTINGS_STARTUP_FAILED));
    }
}

/// Said once after a run that ended with a crash report.
void ShowCrashNotice(const std::wstring& directory) {
    namespace diagnostics = easnap::platform::diagnostics;
    namespace strings = easnap::strings;
    if (!diagnostics::PendingCrashReport(directory).has_value()) {
        return;
    }
    diagnostics::AcknowledgeCrashReports(directory);
    easnap::platform::LogMessage(L"The previous run ended with a crash report");
    enum : int { kOpen = 100, kLater = 101 };
    const int choice = easnap::ui::ShowChoice(
        nullptr, strings::Get(IDS_CRASH_TEXT), strings::Get(IDS_CRASH_TITLE),
        {{kOpen, strings::Get(IDS_CRASH_OPEN)}, {kLater, strings::Get(IDS_NOT_NOW)}}, kLater, kLater);
    if (choice == kOpen) {
        ::ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void OpenDataFolder() {
    const std::wstring directory = easnap::platform::AppDataDirectory();
    if (directory.empty()) {
        return;
    }
    easnap::platform::EnsureDirectory(directory);
    ::ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowTrayMenu(HWND hwnd) {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    namespace strings = easnap::strings;
    auto& coordinator = CaptureCoordinator::Instance();
    // The tab in the pattern separates the label from the right-aligned shortcut.
    const std::wstring capture_item = g_hotkey_registered
        ? strings::Format(IDS_SHELL_TRAY_CAPTURE, {coordinator.settings().HotkeyDisplayText()})
        : std::wstring(strings::Get(IDS_TRAY_CAPTURE_NO_SHORTCUT));
    const UINT capture_flags = coordinator.state() == SessionState::Idle ? MF_STRING : (MF_STRING | MF_GRAYED);

    ::AppendMenuW(menu, capture_flags, IDM_TRAY_CAPTURE, capture_item.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (coordinator.settings().capture_cursor ? MF_CHECKED : MF_UNCHECKED),
                  IDM_TRAY_CAPTURE_CURSOR, strings::Get(IDS_SHELL_TRAY_INCLUDE_CURSOR));
    if (HMENU appearance = easnap::ui::CreateAppearanceMenu(coordinator.settings().appearance_follows_system)) {
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), strings::Get(IDS_SHELL_TRAY_APPEARANCE));
    }
    // Settings change what a running session was built with: offered while idle.
    ::AppendMenuW(menu, capture_flags, IDM_TRAY_SETTINGS, strings::Get(IDS_TRAY_SETTINGS));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_DATA_FOLDER, strings::Get(IDS_SHELL_TRAY_OPEN_DATA_FOLDER));
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_CHECK_UPDATES, strings::Get(IDS_TRAY_CHECK_UPDATES));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, strings::Get(IDS_SHELL_TRAY_ABOUT));
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, strings::Get(IDS_SHELL_TRAY_EXIT));

    POINT cursor{};
    ::GetCursorPos(&cursor);
    const HWND invoking_focus = ::GetFocus();
    // Required so the menu closes when the user clicks elsewhere.
    ::SetForegroundWindow(hwnd);
    {
        easnap::ui::PopupScope scope(invoking_focus);
        easnap::ui::MenuThemeScope menu_theme(hwnd,menu);
        const UINT command = ::TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
                                                cursor.x, cursor.y, hwnd, nullptr);
        if (command) { ::PostMessageW(hwnd, WM_COMMAND, command, 0); }
    }
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
    ::DestroyMenu(menu);
}

void OnTrayCommand(HWND hwnd, UINT command) {
    auto& coordinator = CaptureCoordinator::Instance();
    switch (command) {
        case IDM_TRAY_CAPTURE:
            coordinator.OnGlobalHotkey();
            break;
        case IDM_TRAY_CAPTURE_CURSOR:
            coordinator.settings().capture_cursor = !coordinator.settings().capture_cursor;
            coordinator.PersistSettings();
            break;
        case IDM_TRAY_OPEN_DATA_FOLDER:
            OpenDataFolder();
            break;
        case IDM_TRAY_SETTINGS:
            ShowSettings(hwnd);
            break;
        case IDM_TRAY_CHECK_UPDATES:
            // The browser opens the releases page. Easnap itself asks nobody.
            ::ShellExecuteW(nullptr, L"open", kReleasesPage, nullptr, nullptr, SW_SHOWNORMAL);
            break;
        case IDM_TRAY_ABOUT:
            ShowAboutDialog(hwnd);
            break;
        case IDM_TRAY_EXIT:
            if (coordinator.ConfirmExit()) { ::DestroyWindow(hwnd); }
            break;
        default:
            break;
    }
}

LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto& coordinator = CaptureCoordinator::Instance();

    if (message == g_taskbar_created_message && g_taskbar_created_message != 0) {
        g_tray.Restore();
        return 0;
    }
    if (message == g_activate_message && g_activate_message != 0) {
        // Easnap was started again: that is a request to capture.
        coordinator.OnGlobalHotkey();
        return 0;
    }

    switch (message) {
        case WM_HOTKEY:
            if (wparam == IDH_EASNAP_CAPTURE) {
                coordinator.OnGlobalHotkey();
            }
            return 0;

        case kMsgTrayIcon:
            switch (LOWORD(lparam)) {
                case WM_LBUTTONUP:
                    coordinator.OnGlobalHotkey();
                    break;
                case WM_CONTEXTMENU:
                case WM_RBUTTONUP:
                    // Version 4 icons report one right-click as both messages;
                    // the second arrives inside the first menu's modal loop.
                    if (!easnap::ui::PopupActive()) { ShowTrayMenu(hwnd); }
                    break;
                default:
                    break;
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wparam) == easnap::ui::kAppearanceLight || LOWORD(wparam) == easnap::ui::kAppearanceDark) {
                coordinator.SetAppearanceTheme(LOWORD(wparam) == easnap::ui::kAppearanceDark
                    ? easnap::app::AppearanceTheme::Dark : easnap::app::AppearanceTheme::Light);
                return 0;
            }
            if (LOWORD(wparam) == easnap::ui::kAppearanceSystem) {
                coordinator.SetAppearanceFollowsSystem();
                return 0;
            }
            OnTrayCommand(hwnd, LOWORD(wparam));
            return 0;

        case easnap::app::kMsgSessionCleanup:
            coordinator.OnDeferredCleanup();
            return 0;

        case easnap::app::kMsgRecordingFinalized:
            coordinator.OnRecordingFinalized(static_cast<UINT_PTR>(wparam));
            return 0;

        case easnap::app::kMsgTrimExportCompleted:
            coordinator.OnTrimExportCompleted(static_cast<uint64_t>(wparam));
            return 0;

        case easnap::app::kMsgScrollshotProgress:
            coordinator.OnScrollshotProgress(static_cast<uint64_t>(wparam));
            return 0;

        case WM_CLIPBOARDUPDATE:
            coordinator.OnClipboardChanged();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            coordinator.OnDisplayChanged();
            return 0;

        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) { coordinator.OnDisplayChanged(); }
            else { coordinator.OnVisualSettingsChanged(); }
            return 0;

        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            coordinator.OnVisualSettingsChanged();
            return 0;

        case WM_TIMER:
            if (wparam == easnap::app::kTimerDeferredCleanup) {
                coordinator.OnDeferredCleanup();
                return 0;
            }
            break;

        case WM_QUERYENDSESSION:
            // Sign-out or shutdown while work would be lost: Windows shows the
            // reason and lets the user decide. A process without a visible
            // window is only listed there when it gives a reason.
            if (const wchar_t* reason = coordinator.ShutdownBlockReason()) {
                ::ShutdownBlockReasonCreate(hwnd, reason);
                return FALSE;
            }
            return TRUE;

        case WM_ENDSESSION:
            ::ShutdownBlockReasonDestroy(hwnd);
            if (wparam != FALSE) {
                coordinator.PrepareForSessionEnd();
                easnap::platform::diagnostics::StopLog();
            }
            return 0;

        case WM_CLOSE:
            if (coordinator.ConfirmExit()) { ::DestroyWindow(hwnd); }
            return 0;

        case WM_DESTROY:
            if (g_hotkey_registered) {
                ::UnregisterHotKey(hwnd, IDH_EASNAP_CAPTURE);
                g_hotkey_registered = false;
            }
            coordinator.PersistSettings();
            coordinator.Shutdown();
            g_tray.Remove();
            ::PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND CreateHostWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostClassName;
    wc.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_EASNAP_APP));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    if (::RegisterClassExW(&wc) == 0) {
        return nullptr;
    }

    // A hidden tool window: never focusable, never on the taskbar or Alt+Tab.
    return ::CreateWindowExW(WS_EX_TOOLWINDOW, kHostClassName, kProductName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                             instance, nullptr);
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR command_line, _In_ int) {
    easnap::platform::RestrictDllSearchPath();
    namespace diagnostics = easnap::platform::diagnostics;
    const std::wstring crash_directory = diagnostics::CrashDirectory();
    diagnostics::InstallCrashHandler(crash_directory, L"easnap");
    // Started by the entry Settings writes for sign-in: stay quiet.
    const bool started_at_sign_in = command_line != nullptr && ::wcsstr(command_line, easnap::platform::kStartupArgument) != nullptr;
    g_activate_message = ::RegisterWindowMessageW(kActivateMessageName);

    // Single instance, so the global shortcut and the notification icon have
    // exactly one owner. Starting Easnap again is a request to capture: the
    // running instance is told, and this process ends.
    const easnap::platform::UniqueHandle instance_mutex{::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex)};
    if (!instance_mutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND running = ::FindWindowW(kHostClassName, kProductName);
        if (running != nullptr && g_activate_message != 0) {
            if (!started_at_sign_in) {
                // This process was just started by the user and may hand the
                // foreground on; without that the workspace opens without the keyboard.
                DWORD running_process = 0;
                ::GetWindowThreadProcessId(running, &running_process);
                ::AllowSetForegroundWindow(running_process);
                ::PostMessageW(running, g_activate_message, 0, 0);
            }
        } else {
            easnap::ui::ShowMessage(nullptr, easnap::strings::Get(IDS_SHELL_ALREADY_RUNNING), kProductName,
                                    MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }

    diagnostics::StartLog(diagnostics::LogDirectory(), L"easnap");
    const easnap::platform::WindowsVersion windows = easnap::platform::CurrentWindowsVersion();
    easnap::platform::LogMessage(L"Easnap %ls started on Windows %lu.%lu build %lu", EASNAP_VERSION_TEXT_W, windows.major,
                                 windows.minor, windows.build);
    // Windows 10 version 2004. Earlier builds cannot keep Easnap's own windows
    // out of a recording or a scrolling screenshot, and nothing is tested there.
    if (windows.build != 0 && (windows.major < 10 || (windows.major == 10 && windows.build < 19041))) {
        const std::wstring text = easnap::strings::Format(IDS_WINDOWS_TOO_OLD, {std::to_wstring(windows.build)});
        ::MessageBoxW(nullptr, text.c_str(), kProductName, MB_OK | MB_ICONWARNING);
        return 1;
    }
    easnap::platform::EnableDpiAwareness();

    const easnap::platform::ComRuntime com;
    if (!com.ok()) {
        easnap::ui::ShowMessage(nullptr, easnap::strings::Get(IDS_SHELL_COM_FAILED), kProductName,
                      MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND host = CreateHostWindow(instance);
    if (host == nullptr) {
        easnap::ui::ShowMessage(nullptr, easnap::strings::Get(IDS_SHELL_HOST_WINDOW_FAILED), kProductName,
                      MB_OK | MB_ICONERROR);
        return 1;
    }

    auto& coordinator = CaptureCoordinator::Instance();
    if (!coordinator.Initialize(host)) {
        ::DestroyWindow(host);
        return 1;
    }
    // Text shown before this line follows Windows; from here the choice in Settings applies.
    easnap::strings::SetPreference(ToPreference(coordinator.settings().language));

    // Explorer broadcasts this when it restarts and every tray icon is lost.
    g_taskbar_created_message = ::RegisterWindowMessageW(L"TaskbarCreated");
    ::ChangeWindowMessageFilterEx(host, g_taskbar_created_message, MSGFLT_ALLOW, nullptr);

    // The tooltip and the menu say whether the shortcut works, so the icon is
    // added after the attempt to register it.
    const bool shortcut_works = RegisterCaptureHotkey(host);
    if (!g_tray.Add(host, TrayTooltip())) {
        easnap::ui::ShowMessage(nullptr, easnap::strings::Get(IDS_SHELL_TRAY_ICON_FAILED), kProductName,
                      MB_OK | MB_ICONERROR);
    }
    const std::wstring shortcut = coordinator.settings().HotkeyDisplayText();
    if (!shortcut_works) {
        // Another application holds the shortcut. Said in a notification, not
        // in a dialog that would have to be dismissed at every start.
        if (!started_at_sign_in) {
            g_tray.ShowBalloon(kProductName, easnap::strings::Format(IDS_TRAY_SHORTCUT_UNAVAILABLE, {shortcut}).c_str());
        }
    } else if (!coordinator.settings().first_run_notice_shown) {
        // Where Easnap lives and how to start a capture: said once per computer.
        g_tray.ShowBalloon(kProductName, easnap::strings::Format(IDS_SHELL_TRAY_READY_BALLOON, {shortcut}).c_str());
        coordinator.settings().first_run_notice_shown = true;
        coordinator.PersistSettings();
    }

    ShowCrashNotice(crash_directory);
    coordinator.OfferRecoveredRecordings();

    MSG message{};
    BOOL result = FALSE;
    while ((result = ::GetMessageW(&message, nullptr, 0, 0)) != 0) {
        if (result == -1) {
            break;  // Fatal message-queue error.
        }
        if (coordinator.ProcessDialogMessage(message)) {
            continue;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    g_tray.Remove();
    // Release process-wide graphics COM objects while the main apartment is
    // still initialized; the function-static environment is destroyed later.
    easnap::platform::GraphicsEnvironment::Get().Shutdown();
    easnap::platform::LogMessage(L"Easnap exited");
    diagnostics::StopLog();
    return static_cast<int>(message.wParam);
}
