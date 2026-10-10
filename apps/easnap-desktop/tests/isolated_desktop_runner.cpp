// Easnap test tooling - runs one test process on a private Windows desktop.
//
// A desktop created with CreateDesktop is not the input desktop: its windows
// are never shown on the monitor, the user's mouse and keyboard cannot reach
// them, and they cannot take focus or the cursor away from the user. Tests
// that drive production windows with window messages run there unchanged and
// can no longer be disturbed by, or disturb, someone using the computer.
//
// Tests that need the real input queue (SendInput), the composed desktop
// (Desktop Duplication, Windows.Graphics.Capture) or the foreground window
// cannot run this way and stay on the visible desktop.
//
// Usage: easnap_isolated_desktop <test executable> [arguments...]
// Set EASNAP_TEST_ISOLATION=off to run on the visible desktop for debugging.

#include <windows.h>

#include <cstdio>
#include <string>

namespace {

constexpr int kRunnerFailure = 125;

// Standard CommandLineToArgvW-compatible quoting.
std::wstring QuoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }
    std::wstring quoted = L"\"";
    for (auto it = argument.begin();; ++it) {
        std::size_t backslashes = 0;
        while (it != argument.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == argument.end()) {
            quoted.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
        } else {
            quoted.append(backslashes, L'\\');
        }
        quoted.push_back(*it);
    }
    quoted.push_back(L'"');
    return quoted;
}

bool IsolationDisabled() {
    wchar_t value[8]{};
    const DWORD length = ::GetEnvironmentVariableW(L"EASNAP_TEST_ISOLATION", value, 8);
    return length > 0 && length < 8 && ::lstrcmpiW(value, L"off") == 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::fputs("usage: easnap_isolated_desktop <test executable> [arguments...]\n", stderr);
        return kRunnerFailure;
    }
    std::wstring command_line;
    for (int index = 1; index < argc; ++index) {
        if (index > 1) {
            command_line.push_back(L' ');
        }
        command_line += QuoteArgument(argv[index]);
    }

    HDESK desktop = nullptr;
    std::wstring desktop_path;
    if (!IsolationDisabled()) {
        wchar_t station[128]{};
        DWORD needed = 0;
        if (!::GetUserObjectInformationW(::GetProcessWindowStation(), UOI_NAME, station,
                                         sizeof(station), &needed)) {
            std::fprintf(stderr, "isolated desktop: window station name unavailable (%lu)\n",
                         ::GetLastError());
            return kRunnerFailure;
        }
        wchar_t name[64]{};
        ::swprintf_s(name, L"EasnapTest-%lu-%llu", ::GetCurrentProcessId(), ::GetTickCount64());
        desktop = ::CreateDesktopW(name, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (desktop == nullptr) {
            std::fprintf(stderr, "isolated desktop: CreateDesktop failed (%lu)\n", ::GetLastError());
            return kRunnerFailure;
        }
        desktop_path = std::wstring(station) + L"\\" + name;
        std::printf("Isolated desktop: %ls (not visible, no user input)\n", desktop_path.c_str());
        std::fflush(stdout);
    }

    // A CTest timeout kills only this runner. The job takes the test and any
    // helper it started with it instead of leaving them on a hidden desktop.
    const HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.lpDesktop = desktop_path.empty() ? nullptr : desktop_path.data();
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED,
                          nullptr, nullptr, &startup, &process)) {
        std::fprintf(stderr, "isolated desktop: could not start the test (%lu)\n", ::GetLastError());
        if (desktop != nullptr) {
            ::CloseDesktop(desktop);
        }
        return kRunnerFailure;
    }
    if (job != nullptr) {
        ::AssignProcessToJobObject(job, process.hProcess);
    }
    ::ResumeThread(process.hThread);
    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = static_cast<DWORD>(kRunnerFailure);
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    if (job != nullptr) {
        ::CloseHandle(job);
    }
    if (desktop != nullptr) {
        ::CloseDesktop(desktop);
    }
    return static_cast<int>(exit_code);
}
