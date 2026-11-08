// Easnap - log file and crash reports.
//
// Both stay on this computer, under %LOCALAPPDATA%\Easnap. Nothing here opens
// a network connection. Log lines carry no user content: no path, file name,
// window title or text the user typed. A crash report is a minidump without
// the process's heap; it holds the call stacks of every thread, which can
// contain fragments of what the process was handling at that moment.
#pragma once

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

namespace easnap::platform::diagnostics {

struct LogOptions {
    /// A log file that has reached this size is set aside and a new one begun.
    uint64_t max_bytes{uint64_t{1024} * 1024};
    /// Files kept in all: the current one and `kept_files - 1` earlier ones.
    int kept_files{3};
};

/// %LOCALAPPDATA%\Easnap\logs and ...\crashes. Empty when the profile folder
/// cannot be resolved.
[[nodiscard]] std::wstring LogDirectory();
[[nodiscard]] std::wstring CrashDirectory();

/// Appends every `LogMessage` line of this process to `<directory>\<name>.log`
/// with a UTC time and the thread id. Earlier files are `<name>.1.log`,
/// `<name>.2.log`, oldest last. False when the file cannot be opened; logging
/// to the debugger continues either way.
bool StartLog(const std::wstring& directory, const wchar_t* name, const LogOptions& options = {});
void StopLog() noexcept;

/// From now on an unhandled exception, `std::terminate`, `abort`, a pure
/// virtual call or an invalid CRT parameter on any thread writes
/// `<directory>\<name>-<UTC time to the millisecond>-<pid>.dmp` (with a
/// counter when that name exists; a report never replaces another) and ends
/// the process with the failure's code. At most `kept_reports` reports stay in the directory.
/// A line in the log says whether the report was written and, if not, at
/// which step and with which error; when the log cannot take it, the line
/// goes to the debugger. Call once, as early as possible.
void InstallCrashHandler(const std::wstring& directory, const wchar_t* name, int kept_reports = 3) noexcept;

/// The step at which `InstallCrashHandler` failed, if one did.
enum class CrashHandlerStep : uint8_t {
    None,    ///< Installed; a failure writes a report.
    Folder,  ///< Not called yet, or no usable folder or name: nothing installed.
    Writer,  ///< dbghelp.dll or its MiniDumpWriteDump could not be loaded: installed,
             ///< a failure ends the process with its code and is logged, without a report.
    Events,  ///< Nothing installed.
    Thread,  ///< Nothing installed.
};

struct CrashHandlerStatus {
    CrashHandlerStep failed{CrashHandlerStep::Folder};
    /// The Windows error code of that step; 0 where there is none.
    DWORD error{0};
};

[[nodiscard]] CrashHandlerStatus CrashHandlerInstallStatus() noexcept;
/// Logs `CrashHandlerInstallStatus` in one line. The handler is installed
/// before the log starts, so call this once the log runs.
void LogCrashHandlerStatus();

struct CrashReport {
    std::wstring path;
    FILETIME written{};
};

/// The newest report in `directory` that has not been acknowledged yet.
[[nodiscard]] std::optional<CrashReport> PendingCrashReport(const std::wstring& directory);
/// Marks every report now in `directory` as seen.
void AcknowledgeCrashReports(const std::wstring& directory);

}  // namespace easnap::platform::diagnostics
