// Easnap - log file and crash reports.
#include "platform/diagnostics.h"

#include <dbghelp.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <vector>

#include "platform/platform_utils.h"

namespace easnap::platform::diagnostics {
namespace {

// --- Log file -----------------------------------------------------------------

struct LogState {
    SRWLOCK lock = SRWLOCK_INIT;
    HANDLE file{INVALID_HANDLE_VALUE};
    std::wstring directory;
    std::wstring name;
    uint64_t size{0};
    LogOptions options;
};

/// Never destroyed: a worker may log while the process is exiting.
LogState& Log() {
    static LogState* const state = new LogState;
    return *state;
}

[[nodiscard]] std::wstring LogPath(const LogState& state, int index) {
    std::wstring path = state.directory + L"\\" + state.name;
    if (index > 0) { path += L"." + std::to_wstring(index); }
    return path + L".log";
}

/// Lock held.
void OpenCurrent(LogState& state, bool truncate) noexcept {
    state.file = ::CreateFileW(LogPath(state, 0).c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               truncate ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    state.size = 0;
    LARGE_INTEGER size{};
    if (state.file != INVALID_HANDLE_VALUE && ::GetFileSizeEx(state.file, &size) != FALSE) {
        state.size = static_cast<uint64_t>(size.QuadPart);
    }
}

/// Lock held. The current file becomes `.1`, `.1` becomes `.2`, and the
/// oldest one is dropped.
void Rotate(LogState& state) noexcept {
    if (state.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
    const int last = std::max(1, state.options.kept_files) - 1;
    if (last == 0) {
        OpenCurrent(state, true);
        return;
    }
    ::DeleteFileW(LogPath(state, last).c_str());
    for (int index = last - 1; index >= 0; --index) {
        ::MoveFileExW(LogPath(state, index).c_str(), LogPath(state, index + 1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    OpenCurrent(state, true);
}

/// One line as UTF-8 with its time and thread. Returns the byte count.
[[nodiscard]] int FormatLine(const wchar_t* text, char* out, int capacity) noexcept {
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    int length = std::snprintf(out, static_cast<size_t>(capacity), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ [%lu] ",
                               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                               now.wMilliseconds, ::GetCurrentThreadId());
    if (length < 0 || length >= capacity) { return 0; }
    // A UTF-16 unit needs at most three bytes; never cut a surrogate pair.
    int units = static_cast<int>(::wcslen(text));
    while (units > 0 && (text[units - 1] == L'\n' || text[units - 1] == L'\r')) { --units; }
    units = std::min(units, (capacity - length - 2) / 3);
    if (units > 0 && text[units - 1] >= 0xD800 && text[units - 1] <= 0xDBFF) { --units; }
    if (units > 0) {
        length += ::WideCharToMultiByte(CP_UTF8, 0, text, units, out + length, capacity - length - 2, nullptr, nullptr);
    }
    out[length++] = '\r';
    out[length++] = '\n';
    return length;
}

void WriteLine(const wchar_t* text) noexcept {
    char line[3200];
    const int length = FormatLine(text, line, static_cast<int>(sizeof(line)));
    if (length <= 0) { return; }
    LogState& state = Log();
    ::AcquireSRWLockExclusive(&state.lock);
    // A full file is set aside before the next line is written, not after the
    // line that filled it: the newest line is always in the current file.
    if (state.file != INVALID_HANDLE_VALUE && state.size >= state.options.max_bytes) { Rotate(state); }
    if (state.file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (::WriteFile(state.file, line, static_cast<DWORD>(length), &written, nullptr) != FALSE) {
            state.size += written;
        }
    }
    ::ReleaseSRWLockExclusive(&state.lock);
}

// --- Crash reports ----------------------------------------------------------------

constexpr DWORD kTerminateCode = 0xE0E50001;         ///< abort / std::terminate
constexpr DWORD kPureCallCode = 0xE0E50002;          ///< pure virtual call
constexpr DWORD kInvalidParameterCode = 0xE0E50003;  ///< invalid CRT parameter
constexpr wchar_t kSeenFile[] = L"last-seen.txt";

using WriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                        PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

struct CrashState {
    wchar_t directory[MAX_PATH]{};
    wchar_t name[32]{};
    WriteDumpFunction write_dump{nullptr};
    HANDLE request{nullptr};
    HANDLE done{nullptr};
    EXCEPTION_POINTERS* pointers{nullptr};
    DWORD thread_id{0};
    std::atomic<long> entered{0};
};

CrashState& Crash() {
    static CrashState* const state = new CrashState;
    return *state;
}

/// Runs on its own thread, so a stack overflow can still be reported.
DWORD WINAPI ReportThread(void*) noexcept {
    CrashState& state = Crash();
    if (::WaitForSingleObject(state.request, INFINITE) != WAIT_OBJECT_0) { return 0; }
    SYSTEMTIME now{};
    ::GetSystemTime(&now);
    wchar_t path[MAX_PATH + 96]{};
    ::swprintf(path, ARRAYSIZE(path), L"%s\\%s-%04u%02u%02u-%02u%02u%02u-%lu.dmp", state.directory, state.name,
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, ::GetCurrentProcessId());
    bool written = false;
    HANDLE file = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{};
        exception.ThreadId = state.thread_id;
        exception.ExceptionPointers = state.pointers;
        exception.ClientPointers = FALSE;
        // Stacks, modules and thread states. No heap, no handle data.
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        written = state.write_dump != nullptr &&
                  state.write_dump(::GetCurrentProcess(), ::GetCurrentProcessId(), file, type,
                                   state.pointers != nullptr ? &exception : nullptr, nullptr, nullptr) != FALSE;
        ::CloseHandle(file);
        if (!written) { ::DeleteFileW(path); }
    }
    // The crashing thread may hold the log lock: write only if it is free.
    LogState& log = Log();
    if (::TryAcquireSRWLockExclusive(&log.lock) != FALSE) {
        if (log.file != INVALID_HANDLE_VALUE) {
            const DWORD code = state.pointers != nullptr && state.pointers->ExceptionRecord != nullptr
                                   ? state.pointers->ExceptionRecord->ExceptionCode : 0;
            wchar_t text[160]{};
            ::swprintf(text, ARRAYSIZE(text), L"Fatal error 0x%08lX on thread %lu; crash report %s", code,
                       state.thread_id, written ? L"written" : L"could not be written");
            char line[512];
            const int length = FormatLine(text, line, static_cast<int>(sizeof(line)));
            DWORD bytes = 0;
            if (length > 0) { ::WriteFile(log.file, line, static_cast<DWORD>(length), &bytes, nullptr); }
            ::FlushFileBuffers(log.file);
        }
        ::ReleaseSRWLockExclusive(&log.lock);
    }
    ::SetEvent(state.done);
    return 0;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* pointers) noexcept {
    CrashState& state = Crash();
    if (state.entered.exchange(1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;  // A second failure while the first is being reported.
    }
    state.pointers = pointers;
    state.thread_id = ::GetCurrentThreadId();
    if (state.request != nullptr && state.done != nullptr) {
        ::SetEvent(state.request);
        ::WaitForSingleObject(state.done, 20000);
    }
    const DWORD code = pointers != nullptr && pointers->ExceptionRecord != nullptr
                           ? pointers->ExceptionRecord->ExceptionCode : kTerminateCode;
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

/// Turns a failure the runtime detected into an exception with a context, so
/// the report shows where it happened.
[[noreturn]] void FailWith(DWORD code) noexcept {
    __try {
        ::RaiseException(code, EXCEPTION_NONCONTINUABLE, 0, nullptr);
    } __except (Filter(GetExceptionInformation())) {
    }
    ::TerminateProcess(::GetCurrentProcess(), code);
    for (;;) { ::Sleep(INFINITE); }
}

void OnAbort(int) noexcept { FailWith(kTerminateCode); }
void OnTerminate() noexcept { FailWith(kTerminateCode); }
void OnPureCall() noexcept { FailWith(kPureCallCode); }
void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) noexcept {
    FailWith(kInvalidParameterCode);
}

struct ReportFile {
    std::wstring name;
    FILETIME written{};
};

/// Reports in `directory`, newest first.
[[nodiscard]] std::vector<ReportFile> ListReports(const std::wstring& directory) {
    std::vector<ReportFile> reports;
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW((directory + L"\\*.dmp").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) { return reports; }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            reports.push_back({data.cFileName, data.ftLastWriteTime});
        }
    } while (::FindNextFileW(find, &data) != FALSE);
    ::FindClose(find);
    std::sort(reports.begin(), reports.end(), [](const ReportFile& a, const ReportFile& b) {
        const LONG order = ::CompareFileTime(&a.written, &b.written);
        return order != 0 ? order > 0 : a.name > b.name;
    });
    return reports;
}

[[nodiscard]] std::wstring ReadSeen(const std::wstring& directory) {
    HANDLE file = ::CreateFileW((directory + L"\\" + kSeenFile).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return {}; }
    wchar_t buffer[MAX_PATH]{};
    DWORD read = 0;
    ::ReadFile(file, buffer, sizeof(buffer) - sizeof(wchar_t), &read, nullptr);
    ::CloseHandle(file);
    return std::wstring(buffer, read / sizeof(wchar_t));
}

[[nodiscard]] std::wstring Subdirectory(const wchar_t* name) {
    const std::wstring root = AppDataDirectory();
    if (root.empty()) { return {}; }
    const std::wstring directory = root + L"\\" + name;
    return EnsureDirectory(directory) ? directory : std::wstring();
}

}  // namespace

std::wstring LogDirectory() { return Subdirectory(L"logs"); }
std::wstring CrashDirectory() { return Subdirectory(L"crashes"); }

bool StartLog(const std::wstring& directory, const wchar_t* name, const LogOptions& options) {
    if (directory.empty() || name == nullptr || *name == L'\0' || !EnsureDirectory(directory)) { return false; }
    LogState& state = Log();
    ::AcquireSRWLockExclusive(&state.lock);
    if (state.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
    state.directory = directory;
    state.name = name;
    state.options = options;
    state.options.max_bytes = std::max<uint64_t>(state.options.max_bytes, 1024);
    OpenCurrent(state, false);
    if (state.file != INVALID_HANDLE_VALUE && state.size >= state.options.max_bytes) { Rotate(state); }
    const bool opened = state.file != INVALID_HANDLE_VALUE;
    ::ReleaseSRWLockExclusive(&state.lock);
    SetLogSink(opened ? &WriteLine : nullptr);
    return opened;
}

void StopLog() noexcept {
    SetLogSink(nullptr);
    LogState& state = Log();
    ::AcquireSRWLockExclusive(&state.lock);
    if (state.file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(state.file);
        state.file = INVALID_HANDLE_VALUE;
    }
    ::ReleaseSRWLockExclusive(&state.lock);
}

void InstallCrashHandler(const std::wstring& directory, const wchar_t* name, int kept_reports) noexcept {
    CrashState& state = Crash();
    if (state.request != nullptr || directory.empty() || directory.size() >= ARRAYSIZE(state.directory) ||
        name == nullptr || ::wcslen(name) >= ARRAYSIZE(state.name) || !EnsureDirectory(directory)) {
        return;
    }
    ::wcscpy_s(state.directory, directory.c_str());
    ::wcscpy_s(state.name, name);
    // Old reports go now; the newest ones stay for the person to look at.
    const std::vector<ReportFile> reports = ListReports(directory);
    for (size_t index = static_cast<size_t>(std::max(kept_reports, 1)); index < reports.size(); ++index) {
        ::DeleteFileW((directory + L"\\" + reports[index].name).c_str());
    }
    // Loaded now, from the system directory: nothing is resolved while failing.
    if (HMODULE library = ::LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        state.write_dump = reinterpret_cast<WriteDumpFunction>(
            reinterpret_cast<void*>(::GetProcAddress(library, "MiniDumpWriteDump")));
    }
    state.request = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    state.done = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = state.request != nullptr && state.done != nullptr
                        ? ::CreateThread(nullptr, 256 * 1024, ReportThread, nullptr, 0, nullptr) : nullptr;
    if (thread == nullptr) {
        return;  // Windows Error Reporting keeps its default behaviour.
    }
    ::CloseHandle(thread);
    ::SetUnhandledExceptionFilter(Filter);
    // `std::terminate` installs per thread; its default ends in abort, which
    // raises SIGABRT for the whole process.
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbort);
    ::_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    ::_set_purecall_handler(OnPureCall);
    ::_set_invalid_parameter_handler(OnInvalidParameter);
}

std::optional<CrashReport> PendingCrashReport(const std::wstring& directory) {
    if (directory.empty()) { return std::nullopt; }
    const std::vector<ReportFile> reports = ListReports(directory);
    if (reports.empty() || reports.front().name == ReadSeen(directory)) { return std::nullopt; }
    return CrashReport{directory + L"\\" + reports.front().name, reports.front().written};
}

void AcknowledgeCrashReports(const std::wstring& directory) {
    if (directory.empty()) { return; }
    const std::vector<ReportFile> reports = ListReports(directory);
    if (reports.empty()) { return; }
    HANDLE file = ::CreateFileW((directory + L"\\" + kSeenFile).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return; }
    DWORD written = 0;
    const std::wstring& name = reports.front().name;
    ::WriteFile(file, name.data(), static_cast<DWORD>(name.size() * sizeof(wchar_t)), &written, nullptr);
    ::CloseHandle(file);
}

}  // namespace easnap::platform::diagnostics
