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
#include "platform/platform_utils.h"
#include "resource.h"

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
        icon_ = easnap::platform::LoadAppIcon(::GetSystemMetrics(SM_CXSMICON));

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

TrayIcon g_tray;
UINT g_taskbar_created_message = 0;
bool g_hotkey_registered = false;

std::wstring TrayTooltip() {
    return std::wstring(kProductName) + L" - " + CaptureCoordinator::Instance().settings().HotkeyDisplayText() +
           L" to capture";
}

bool RegisterCaptureHotkey(HWND hwnd) {
    const auto& settings = CaptureCoordinator::Instance().settings();
    if (::RegisterHotKey(hwnd, IDH_EASNAP_CAPTURE, settings.hotkey_modifiers | MOD_NOREPEAT,
                         settings.hotkey_virtual_key) != FALSE) {
        g_hotkey_registered = true;
        return true;
    }
    easnap::platform::LogMessage(L"RegisterHotKey failed (error %lu)", ::GetLastError());
    return false;
}

void ShowAboutDialog(HWND owner) {
    const std::wstring text =
        std::wstring(kProductName) + L" " + std::to_wstring(EASNAP_VERSION_MAJOR) + L"." +
        std::to_wstring(EASNAP_VERSION_MINOR) + L"." + std::to_wstring(EASNAP_VERSION_PATCH) +
        L"\n\nA local-first screen capture tool for Windows 10.\n"
        L"Screenshots never leave this computer: Easnap makes no network calls and collects no telemetry.\n\n"
        L"Capture shortcut: " +
        CaptureCoordinator::Instance().settings().HotkeyDisplayText() +
        L"\n\nMIT licensed. See LICENSE and SECURITY.md in the repository.";
    easnap::ui::ShowMessage(owner, text.c_str(), L"About Easnap", MB_OK | MB_ICONINFORMATION);
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

    auto& coordinator = CaptureCoordinator::Instance();
    const std::wstring capture_item = L"Capture\t" + coordinator.settings().HotkeyDisplayText();
    const UINT capture_flags = coordinator.state() == SessionState::Idle ? MF_STRING : (MF_STRING | MF_GRAYED);

    ::AppendMenuW(menu, capture_flags, IDM_TRAY_CAPTURE, capture_item.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING | (coordinator.settings().capture_cursor ? MF_CHECKED : MF_UNCHECKED),
                  IDM_TRAY_CAPTURE_CURSOR, L"Include mouse cursor");
    if (HMENU appearance = easnap::ui::CreateAppearanceMenu()) {
        ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance), L"Appearance");
    }
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN_DATA_FOLDER, L"Open data folder");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT, L"About Easnap");
    ::AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit");

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
        case IDM_TRAY_ABOUT:
            ShowAboutDialog(hwnd);
            break;
        case IDM_TRAY_EXIT:
            ::DestroyWindow(hwnd);
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

        case WM_ENDSESSION:
            if (wparam != FALSE) {
                coordinator.PersistSettings();
                coordinator.Shutdown();
            }
            return 0;

        case WM_CLOSE:
            ::DestroyWindow(hwnd);
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

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    easnap::platform::RestrictDllSearchPath();
    // Single instance: a second launch just tells the user and exits, so the
    // global shortcut and the notification icon always have exactly one owner.
    const easnap::platform::UniqueHandle instance_mutex{::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex)};
    if (!instance_mutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        easnap::ui::ShowMessage(nullptr, L"Easnap is already running. Look for its icon in the notification area.", kProductName,
                      MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    easnap::platform::EnableDpiAwareness();

    const easnap::platform::ComRuntime com;
    if (!com.ok()) {
        easnap::ui::ShowMessage(nullptr, L"Easnap could not initialise COM and cannot continue.", kProductName,
                      MB_OK | MB_ICONERROR);
        return 1;
    }

    HWND host = CreateHostWindow(instance);
    if (host == nullptr) {
        easnap::ui::ShowMessage(nullptr, L"Easnap could not create its message window.", kProductName, MB_OK | MB_ICONERROR);
        return 1;
    }

    auto& coordinator = CaptureCoordinator::Instance();
    if (!coordinator.Initialize(host)) {
        ::DestroyWindow(host);
        return 1;
    }

    // Explorer broadcasts this when it restarts and every tray icon is lost.
    g_taskbar_created_message = ::RegisterWindowMessageW(L"TaskbarCreated");
    ::ChangeWindowMessageFilterEx(host, g_taskbar_created_message, MSGFLT_ALLOW, nullptr);

    if (!g_tray.Add(host, TrayTooltip())) {
        easnap::ui::ShowMessage(nullptr, L"Easnap could not create its notification-area icon.", kProductName,
                      MB_OK | MB_ICONERROR);
    }

    if (!RegisterCaptureHotkey(host)) {
        const std::wstring text = L"Easnap could not register " +
                                  coordinator.settings().HotkeyDisplayText() +
                                  L". Another application is already using that shortcut.\n\n"
                                  L"You can still start a capture from the notification-area icon.";
        easnap::ui::ShowMessage(nullptr, text.c_str(), kProductName, MB_OK | MB_ICONWARNING);
    } else {
        g_tray.ShowBalloon(kProductName,
                           (L"Ready. Press " + coordinator.settings().HotkeyDisplayText() + L" to capture.").c_str());
    }

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
    return static_cast<int>(message.wParam);
}
