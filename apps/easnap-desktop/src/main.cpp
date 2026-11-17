#include "ui/message_dialog.h"
// Easnap - application entry point.
//
// Easnap is a tray-resident tool: there is no main window. A hidden top-level
// tool window (it must receive broadcasts such as TaskbarCreated) owns the
// notification icon, the global shortcut and the single message loop that
// every UI surface (overlay, editor) is pumped from.
//
// Design rules enforced here:
//   * Exactly one instance may run, so the global shortcut has a single owner.
//   * While idle the process holds no capture, encoder or renderer resources.
//   * Windows are always destroyed outside their own window procedure, via the
//     deferred kMsgSessionCleanup message.
#include <windows.h>
// windows.h comes before the shell headers below, which depend on its types.
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "app/app_settings.h"
#include "app/command_line.h"
#include "app/external_video.h"
#include "app/session_state.h"
#include "app/tray_shell.h"
#include "app/video_handover.h"
#include "media/video_probe.h"
#include "platform/app_messages.h"
#include "platform/diagnostics.h"
#include "platform/graphics_device.h"
#include "platform/platform_utils.h"
#include "platform/system_info.h"
#include "resource.h"
#include "strings/strings.h"

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
using easnap::app::kProductName;
using easnap::app::OnTrayIconMessage;
using easnap::app::OnTrayMenuCommand;
using easnap::app::RegisterCaptureHotkey;
using easnap::app::ShellTrayIcon;
using easnap::app::TellSettingsNotSaved;
using easnap::app::ToLanguagePreference;
using easnap::app::TrayTooltip;
using easnap::app::UnregisterCaptureHotkey;

/// A second Easnap.exe finds the running one's host window by this class: to
/// ask for a capture, or to hand it a video to open (ADR-040).
constexpr wchar_t kHostClassName[] = L"EasnapHostWindow";
constexpr wchar_t kSingleInstanceMutex[] = L"Local\\Easnap.SingleInstance";
/// How long a second Easnap.exe waits for the running one to take a video.
constexpr std::chrono::milliseconds kHandOverTimeout{5000};

/// A second launch without a file posts this to the running instance: start a
/// capture.
constexpr wchar_t kActivateMessageName[] = L"Easnap.StartCapture";

UINT g_taskbar_created_message = 0;
UINT g_activate_message = 0;
/// A video to open, from this process's command line or handed over by a
/// second Easnap.exe; acted on from the host window's own message.
easnap::app::OpenVideoRequests g_video_requests;

/// Said once after a run that ended with a crash report.
void ShowCrashNotice(const std::wstring& directory) {
    namespace diagnostics = easnap::platform::diagnostics;
    namespace strings = easnap::strings;
    if (!diagnostics::PendingCrashReport(directory).has_value()) {
        return;
    }
    diagnostics::AcknowledgeCrashReports(directory);
    easnap::platform::LogMessage(L"The previous run ended with a crash report");
    enum : int { kOpen = 100,
                 kLater = 101 };
    const int choice = easnap::ui::ShowChoice(
        nullptr, strings::Get(IDS_CRASH_TEXT), strings::Get(IDS_CRASH_TITLE),
        {{kOpen, strings::Get(IDS_CRASH_OPEN)}, {kLater, strings::Get(IDS_NOT_NOW)}}, kLater, kLater);
    if (choice == kOpen) {
        ::ShellExecuteW(nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

/// The host's own timers; nothing for a timer it does not know.
std::optional<LRESULT> OnHostTimer(WPARAM wparam) {
    auto& coordinator = CaptureCoordinator::Instance();
    if (wparam == easnap::app::kTimerDeferredCleanup) {
        coordinator.OnDeferredCleanup();
        return 0;
    }
    if (wparam == easnap::app::kTimerVideoProbeRetry) {
        coordinator.OnVideoProbeTimer();
        return 0;
    }
    return std::nullopt;
}

LRESULT OnHostQueryEndSession(HWND hwnd) {
    auto& coordinator = CaptureCoordinator::Instance();
    // Sign-out or shutdown while work would be lost: Windows shows the
    // reason and lets the user decide. A process without a visible
    // window is only listed there when it gives a reason.
    if (const wchar_t* reason = coordinator.ShutdownBlockReason()) {
        ::ShutdownBlockReasonCreate(hwnd, reason);
        return FALSE;
    }
    return TRUE;
}

LRESULT OnHostEndSession(HWND hwnd, WPARAM wparam) {
    auto& coordinator = CaptureCoordinator::Instance();
    ::ShutdownBlockReasonDestroy(hwnd);
    if (wparam != FALSE) {
        coordinator.PrepareForSessionEnd();
        easnap::platform::diagnostics::StopLog();
    }
    return 0;
}

LRESULT OnHostDestroy(HWND hwnd) {
    auto& coordinator = CaptureCoordinator::Instance();
    UnregisterCaptureHotkey(hwnd);
    coordinator.PersistSettings(CaptureCoordinator::IfNotSaved::LogOnly);
    coordinator.Shutdown();
    ShellTrayIcon().Remove();
    ::PostQuitMessage(0);
    return 0;
}

LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto& coordinator = CaptureCoordinator::Instance();

    if (message == g_taskbar_created_message && g_taskbar_created_message != 0) {
        ShellTrayIcon().Restore();
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

        case easnap::app::kMsgTrayIcon:
            return OnTrayIconMessage(hwnd, lparam);

        case WM_COMMAND:
            return OnTrayMenuCommand(hwnd, wparam);

        case easnap::app::kMsgSessionCleanup:
            coordinator.OnDeferredCleanup();
            return 0;

        case easnap::app::kMsgRecordingFinalized:
            coordinator.OnRecordingFinalized(static_cast<UINT_PTR>(wparam));
            return 0;

        case easnap::app::kMsgTrimExportCompleted:
            coordinator.OnTrimExportCompleted(static_cast<uint64_t>(wparam));
            return 0;

        case easnap::app::kMsgVideoProbeCompleted:
            coordinator.OnVideoProbeCompleted(static_cast<UINT_PTR>(wparam));
            return 0;

        case WM_COPYDATA:
            // Any program on the desktop can send this. It is checked and only
            // kept here; the work happens on the message posted below.
            return g_video_requests.OnCopyData(hwnd, reinterpret_cast<const COPYDATASTRUCT*>(lparam),
                                               coordinator.AcceptsVideoRequest())
                       ? TRUE
                       : FALSE;

        case easnap::app::kMsgOpenVideoRequest:
            if (const std::optional<std::wstring> path = g_video_requests.Take()) { coordinator.OpenVideo(*path); }
            return 0;

        case easnap::app::kMsgScrollshotProgress:
            coordinator.OnScrollshotProgress(static_cast<uint64_t>(wparam));
            return 0;

        case easnap::app::kMsgSettingsNotSaved:
            // Asked again: a session or a dialog may have opened since the post.
            if (coordinator.SettingsNoticeDue()) { TellSettingsNotSaved(); }
            return 0;
        case easnap::platform::kMsgImageExportCompleted:
            coordinator.OnImageExportCompleted(static_cast<UINT_PTR>(wparam));
            return 0;

        case WM_CLIPBOARDUPDATE:
            coordinator.OnClipboardChanged();
            return 0;

        case WM_DISPLAYCHANGE:
        case WM_DPICHANGED:
            coordinator.OnDisplayChanged();
            return 0;

        case WM_SETTINGCHANGE:
            if (wparam == SPI_SETWORKAREA) {
                coordinator.OnDisplayChanged();
            } else {
                coordinator.OnVisualSettingsChanged();
            }
            return 0;

        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            coordinator.OnVisualSettingsChanged();
            return 0;

        case WM_TIMER:
            if (const std::optional<LRESULT> result = OnHostTimer(wparam)) { return *result; }
            break;

        case WM_QUERYENDSESSION:
            return OnHostQueryEndSession(hwnd);

        case WM_ENDSESSION:
            return OnHostEndSession(hwnd, wparam);

        case WM_CLOSE:
            if (coordinator.ConfirmExit()) { ::DestroyWindow(hwnd); }
            return 0;

        case WM_DESTROY:
            return OnHostDestroy(hwnd);

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

struct ArgumentsDeleter {
    void operator()(LPWSTR* arguments) const noexcept { ::LocalFree(static_cast<HLOCAL>(arguments)); }
};

/// What this start was asked for (ADR-040); nothing when Windows cannot split
/// the command line.
easnap::app::CommandLineRequest ReadCommandLine() {
    int count = 0;
    const std::unique_ptr<LPWSTR, ArgumentsDeleter> arguments(::CommandLineToArgvW(::GetCommandLineW(), &count));
    if (!arguments) {
        easnap::app::CommandLineRequest request;
        request.bad = true;
        return request;
    }
    return easnap::app::ParseCommandLine(count, arguments.get());
}

/// A second Easnap.exe was given a video: the running one opens it. This
/// process was started by the user, so it, and not the running Easnap, says
/// why nothing opens. The running Easnap checks the request's form, and its
/// probe checks the file again on a worker.
void HandOverVideo(const std::wstring& path) {
    namespace app = easnap::app;
    namespace strings = easnap::strings;
    const easnap::media::VideoRefusal location = easnap::media::CheckVideoLocation(path);
    if (location != easnap::media::VideoRefusal::None) {
        // The answer the running Easnap's probe would give.
        easnap::media::VideoProbeResult refused;
        refused.reason = location;
        easnap::ui::ShowErrorMessage(
            nullptr, strings::Format(app::VideoRefusalMessageId(refused), {app::VideoFileDisplayName(path)}).c_str());
        return;
    }
    switch (app::HandOverOpenVideo(kHostClassName, path, kHandOverTimeout)) {
        case app::HandOverResult::Taken:
            break;
        case app::HandOverResult::Refused:
            easnap::ui::ShowErrorMessage(
                nullptr, strings::Format(IDS_VIDEO_HANDOVER_BUSY, {app::VideoFileDisplayName(path)}).c_str());
            break;
        case app::HandOverResult::NoRecipient:
            easnap::ui::ShowMessage(nullptr, strings::Get(IDS_SHELL_ALREADY_RUNNING), kProductName,
                                    MB_OK | MB_ICONINFORMATION);
            break;
    }
}

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    // A failure is logged once the log is open.
    const bool dll_search_restricted = easnap::platform::RestrictDllSearchPath();
    const DWORD dll_search_error = dll_search_restricted ? ERROR_SUCCESS : ::GetLastError();
    namespace diagnostics = easnap::platform::diagnostics;
    const std::wstring crash_directory = diagnostics::CrashDirectory();
    diagnostics::InstallCrashHandler(crash_directory, L"easnap");
    const easnap::app::CommandLineRequest request = ReadCommandLine();
    // Started by the entry Settings writes for sign-in: stay quiet.
    const bool started_at_sign_in = request.startup;
    g_activate_message = ::RegisterWindowMessageW(kActivateMessageName);

    // Single instance, so the global shortcut and the notification icon have
    // exactly one owner. Starting Easnap again is a request to capture, or,
    // with a video, a request to open it: the running instance is told, and
    // this process ends.
    const easnap::platform::UniqueHandle instance_mutex{::CreateMutexW(nullptr, TRUE, kSingleInstanceMutex)};
    if (!instance_mutex || ::GetLastError() == ERROR_ALREADY_EXISTS) {
        // Anything this process says, it says in the running Easnap's language.
        easnap::strings::SetPreference(ToLanguagePreference(easnap::app::AppSettings::Load().language));
        if (!request.open_path.empty()) {
            HandOverVideo(request.open_path);
            return 0;
        }
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
    diagnostics::LogCrashHandlerStatus();
    if (request.bad) {
        // The arguments themselves may name the user's files: not logged.
        easnap::platform::LogMessage(L"Command line arguments Easnap does not know were ignored");
    }
    // Windows 10 version 2004. Earlier builds cannot keep Easnap's own windows
    // out of a recording or a scrolling screenshot, and nothing is tested there.
    if (windows.build != 0 && (windows.major < 10 || (windows.major == 10 && windows.build < 19041))) {
        const std::wstring text = easnap::strings::Format(IDS_WINDOWS_TOO_OLD, {std::to_wstring(windows.build)});
        ::MessageBoxW(nullptr, text.c_str(), kProductName, MB_OK | MB_ICONWARNING);
        return 1;
    }
    if (!dll_search_restricted) {
        easnap::platform::LogMessage(L"SetDefaultDllDirectories failed (error %lu)", dll_search_error);
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
    coordinator.Initialize(host);
    // Text shown before this line follows Windows; from here the choice in Settings applies.
    easnap::strings::SetPreference(ToLanguagePreference(coordinator.settings().language));

    // Explorer broadcasts this when it restarts and every tray icon is lost.
    g_taskbar_created_message = ::RegisterWindowMessageW(L"TaskbarCreated");
    ::ChangeWindowMessageFilterEx(host, g_taskbar_created_message, MSGFLT_ALLOW, nullptr);

    // The tooltip and the menu say whether the shortcut works, so the icon is
    // added after the attempt to register it.
    const bool shortcut_works = RegisterCaptureHotkey(host);
    if (!ShellTrayIcon().Add(host, TrayTooltip())) {
        easnap::ui::ShowMessage(nullptr, easnap::strings::Get(IDS_SHELL_TRAY_ICON_FAILED), kProductName,
                                MB_OK | MB_ICONERROR);
    }
    const std::wstring shortcut = coordinator.settings().HotkeyDisplayText();
    if (!shortcut_works) {
        // Another application holds the shortcut. Said in a notification, not
        // in a dialog that would have to be dismissed at every start.
        if (!started_at_sign_in) {
            ShellTrayIcon().ShowBalloon(kProductName, easnap::strings::Format(IDS_TRAY_SHORTCUT_UNAVAILABLE, {shortcut}).c_str());
        }
    } else if (!coordinator.settings().first_run_notice_shown) {
        // Where Easnap lives and how to start a capture: said once per computer.
        // Where the settings cannot be saved, that is said instead.
        coordinator.settings().first_run_notice_shown = true;
        if (coordinator.PersistSettings()) {
            ShellTrayIcon().ShowBalloon(kProductName, easnap::strings::Format(IDS_SHELL_TRAY_READY_BALLOON, {shortcut}).c_str());
        } else {
            coordinator.settings().first_run_notice_shown = false;
            TellSettingsNotSaved();
        }
    }

    ShowCrashNotice(crash_directory);
    coordinator.OfferRecoveredRecordings();
    if (!request.open_path.empty()) {
        // Started with a video ("Open with" while Easnap did not run): opened
        // the way a second Easnap.exe's hand-over is, from the host window's
        // own message, once what starting may show has been answered.
        g_video_requests.Queue(host, request.open_path);
    }

    MSG message{};
    BOOL result = FALSE;
    while ((result = ::GetMessageW(&message, nullptr, 0, 0)) != 0) {
        if (result == -1) {
            // The queue cannot be read. WM_DESTROY saves the settings and shuts
            // the coordinator down while COM is still initialised.
            easnap::platform::LogMessage(L"GetMessage failed (error %lu)", ::GetLastError());
            ::DestroyWindow(host);
            break;
        }
        if (coordinator.ProcessDialogMessage(message)) {
            continue;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // WM_DESTROY has done this. Repeated so that the function-static
    // coordinator never shuts down in its destructor, after COM is gone.
    coordinator.Shutdown();
    ShellTrayIcon().Remove();
    // Release process-wide graphics COM objects while the main apartment is
    // still initialized; the function-static environment is destroyed later.
    easnap::platform::GraphicsEnvironment::Get().Shutdown();
    easnap::platform::LogMessage(L"Easnap exited");
    diagnostics::StopLog();
    return result == -1 ? 1 : static_cast<int>(message.wParam);
}
