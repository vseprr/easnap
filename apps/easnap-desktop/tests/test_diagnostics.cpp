// Easnap unit tests - log file and crash reports.
//
// The log is written through the production sink into a directory this test
// creates and removes. Crash reports come from a second process of this
// executable that installs the production handler and then fails on purpose,
// once for every kind of failure the handler claims to catch.

#include "platform/diagnostics.h"
#include "platform/platform_utils.h"

#include <crtdbg.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace easnap::platform;

int g_pass = 0;
int g_fail = 0;
const char* g_case = "";

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL %d [%s]: %s\n", line, g_case, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

// --- Failing on purpose ----------------------------------------------------------

struct Kind {
    const wchar_t* name;
    DWORD exit_code;
};

constexpr Kind kKinds[] = {
    {L"access-violation", 0xC0000005},
    {L"cpp-exception", 0xE06D7363},
    {L"terminate-on-worker", 0xE0E50001},
    {L"noexcept-on-worker", 0xE0E50001},
    {L"abort", 0xE0E50001},
    {L"pure-call", 0xE0E50002},
    {L"invalid-parameter", 0xE0E50003},
    {L"stack-overflow", 0xC00000FD},
};

volatile int g_depth_limit = 0;  // Never reached: the recursion below ends in the guard page.

struct Shape {
    Shape() { Describe(); }
    virtual ~Shape() = default;
    void Describe() { Name(); }
    virtual void Name() = 0;
};
struct Square final : Shape {
    void Name() override {}
};

#pragma warning(push)
#pragma warning(disable : 4717)  // Recursion without an exit is the point.
int Recurse(int depth) {
    volatile char frame[2048];
    frame[0] = static_cast<char>(depth);
    if (depth == g_depth_limit - 1) { return frame[0]; }
    return Recurse(depth + 1) + frame[0];
}
#pragma warning(pop)

__declspec(noinline) void Throw() { throw std::runtime_error("deliberate"); }
void ThrowFromNoexcept() noexcept { Throw(); }

/// `--fail <kind> <directory>`
int RunFailingProcess(const wchar_t* kind, const wchar_t* directory) {
    // Should the handler not take over, end without a system dialog.
    ::SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
#ifdef _DEBUG
    // The debug runtime asks in a dialog before it calls the handler under test.
    ::_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
    ::_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
#endif
    diagnostics::StartLog(directory, L"child");
    diagnostics::InstallCrashHandler(directory, L"test", 3);
    LogMessage(L"About to fail: %ls", kind);
    const std::wstring name = kind;
    if (name == L"access-violation") {
        volatile int* const nowhere = nullptr;
        *nowhere = 1;
    } else if (name == L"cpp-exception") {
        throw std::runtime_error("deliberate");
    } else if (name == L"terminate-on-worker") {
        std::thread([] { std::terminate(); }).join();
    } else if (name == L"noexcept-on-worker") {
        std::thread([] { ThrowFromNoexcept(); }).join();
    } else if (name == L"abort") {
        std::abort();
    } else if (name == L"pure-call") {
        Square square;
    } else if (name == L"invalid-parameter") {
        char four[4];
        volatile size_t capacity = sizeof(four);
        ::strcpy_s(four, capacity, "does not fit in four bytes");
    } else if (name == L"stack-overflow") {
        return Recurse(0);
    }
    return 0;  // Reaching this line means the failure did not happen.
}

// --- Helpers -------------------------------------------------------------------------

std::wstring MakeDirectory() {
    wchar_t temp[MAX_PATH]{};
    ::GetTempPathW(ARRAYSIZE(temp), temp);
    const std::wstring directory = std::wstring(temp) + L"easnap-diagnostics-" + std::to_wstring(::GetCurrentProcessId()) +
                                   L"-" + std::to_wstring(::GetTickCount64());
    return ::CreateDirectoryW(directory.c_str(), nullptr) != FALSE ? directory : std::wstring();
}

std::vector<std::wstring> Files(const std::wstring& directory, const wchar_t* pattern) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW((directory + L"\\" + pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) { return names; }
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) { names.push_back(data.cFileName); }
    } while (::FindNextFileW(find, &data) != FALSE);
    ::FindClose(find);
    return names;
}

void RemoveDirectoryWithFiles(const std::wstring& directory) {
    for (const std::wstring& name : Files(directory, L"*")) { ::DeleteFileW((directory + L"\\" + name).c_str()); }
    ::RemoveDirectoryW(directory.c_str());
}

std::string ReadFile(const std::wstring& path) {
    std::string content;
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { return content; }
    char buffer[8192];
    DWORD read = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) != FALSE && read != 0) { content.append(buffer, read); }
    ::CloseHandle(file);
    return content;
}

std::vector<std::string> Lines(const std::string& content) {
    std::vector<std::string> lines;
    size_t begin = 0;
    while (begin < content.size()) {
        const size_t end = content.find("\r\n", begin);
        if (end == std::string::npos) { lines.push_back(content.substr(begin) + "<unterminated>"); break; }
        lines.push_back(content.substr(begin, end - begin));
        begin = end + 2;
    }
    return lines;
}

/// "2026-10-13T12:34:56.789Z [1234] text"
bool WellFormed(const std::string& line, std::string* text = nullptr) {
    static const char pattern[] = "dddd-dd-ddTdd:dd:dd.dddZ [";
    const size_t prefix = sizeof(pattern) - 1;
    if (line.size() < prefix + 3) { return false; }
    for (size_t i = 0; i < prefix; ++i) {
        if (pattern[i] == 'd' ? (line[i] < '0' || line[i] > '9') : line[i] != pattern[i]) { return false; }
    }
    size_t at = prefix;
    while (at < line.size() && line[at] >= '0' && line[at] <= '9') { ++at; }
    if (at == prefix || at + 1 >= line.size() || line[at] != ']' || line[at + 1] != ' ') { return false; }
    if (text != nullptr) { *text = line.substr(at + 2); }
    return true;
}

// --- Log -----------------------------------------------------------------------------

void TestLog(const std::wstring& directory) {
    g_case = "log";
    CHECK(!diagnostics::StartLog(L"", L"test"));
    CHECK(!diagnostics::StartLog(directory, L""));

    diagnostics::LogOptions options;
    options.max_bytes = 64 * 1024;
    CHECK(diagnostics::StartLog(directory, L"test", options));
    LogMessage(L"first %d", 1);
    LogMessage(L"Türkçe ğışİ \U0001F600 %ls", L"✓");
    LogMessage(L"%ls", std::wstring(3000, L'x').c_str());  // Longer than a line may be.
    LogHresult(L"Operation", E_ACCESSDENIED);
    diagnostics::StopLog();
    LogMessage(L"after the log was stopped");

    const std::wstring path = directory + L"\\test.log";
    std::vector<std::string> lines = Lines(ReadFile(path));
    CHECK(lines.size() == 4);
    if (lines.size() == 4) {
        std::string text;
        CHECK(WellFormed(lines[0], &text) && text == "first 1");
        CHECK(lines[0].find("[" + std::to_string(::GetCurrentThreadId()) + "]") != std::string::npos);
        CHECK(WellFormed(lines[1], &text) &&
              text == "T\xC3\xBCrk\xC3\xA7" "e \xC4\x9F\xC4\xB1\xC5\x9F\xC4\xB0 \xF0\x9F\x98\x80 \xE2\x9C\x93");
        CHECK(WellFormed(lines[2], &text) && text.size() == 1023 && text.find_first_not_of('x') == std::string::npos);
        CHECK(WellFormed(lines[3], &text) && text == "Operation failed (hr=0x80070005)");
    }

    // A second start appends; nothing already written is lost.
    CHECK(diagnostics::StartLog(directory, L"test", options));
    LogMessage(L"second start");
    diagnostics::StopLog();
    lines = Lines(ReadFile(path));
    CHECK(lines.size() == 5 && lines.back().find("second start") != std::string::npos);

    // Rotation under four threads: whole lines only, at most three files, each
    // within its limit plus one line, and the newest line in the current file.
    g_case = "log rotation";
    options.max_bytes = 4096;
    options.kept_files = 3;
    CHECK(diagnostics::StartLog(directory, L"rotating", options));
    std::vector<std::thread> writers;
    for (int writer = 0; writer < 4; ++writer) {
        writers.emplace_back([writer] {
            for (int i = 0; i < 300; ++i) { LogMessage(L"writer %d line %03d of three hundred", writer, i); }
        });
    }
    for (std::thread& writer : writers) { writer.join(); }
    LogMessage(L"the last line");
    diagnostics::StopLog();
    const std::vector<std::wstring> rotated = Files(directory, L"rotating*.log");
    CHECK(rotated.size() == 3);
    CHECK(::GetFileAttributesW((directory + L"\\rotating.3.log").c_str()) == INVALID_FILE_ATTRIBUTES);
    int malformed = 0, oversized = 0;
    size_t kept_lines = 0;
    for (const std::wstring& name : rotated) {
        const std::string content = ReadFile(directory + L"\\" + name);
        if (content.size() > 4096 + 128) { ++oversized; }
        for (const std::string& line : Lines(content)) {
            ++kept_lines;
            std::string text;
            if (!WellFormed(line, &text) ||
                (text != "the last line" && (text.size() != 34 || text.rfind("writer ", 0) != 0))) {
                ++malformed;
            }
        }
    }
    CHECK(malformed == 0 && oversized == 0);
    CHECK(kept_lines > 100 && kept_lines < 1201);  // Older lines went with the files that were dropped.
    const std::vector<std::string> current = Lines(ReadFile(directory + L"\\rotating.log"));
    CHECK(!current.empty() && current.back().find("the last line") != std::string::npos);

    // A file already at its limit is set aside when the log starts.
    options.max_bytes = 1024 * 1024;
    CHECK(diagnostics::StartLog(directory, L"limit", options));
    for (int i = 0; i < 40; ++i) { LogMessage(L"fill %03d ......................................", i); }
    diagnostics::StopLog();
    CHECK(Lines(ReadFile(directory + L"\\limit.log")).size() == 40);  // About 3 kilobytes, far below its megabyte.
    options.max_bytes = 1024;
    CHECK(diagnostics::StartLog(directory, L"limit", options));
    LogMessage(L"fresh file");
    diagnostics::StopLog();
    const std::vector<std::string> fresh = Lines(ReadFile(directory + L"\\limit.log"));
    CHECK(fresh.size() == 1 && fresh[0].find("fresh file") != std::string::npos);
    CHECK(Lines(ReadFile(directory + L"\\limit.1.log")).size() == 40);
}

// --- Crash reports ---------------------------------------------------------------------

bool RunChild(const wchar_t* kind, const std::wstring& directory, DWORD& exit_code) {
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring command = L"\"" + std::wstring(path) + L"\" --fail " + kind + L" \"" + directory + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (::CreateProcessW(path, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                         &process) == FALSE) {
        return false;
    }
    const bool exited = ::WaitForSingleObject(process.hProcess, 60000) == WAIT_OBJECT_0;
    if (!exited) { ::TerminateProcess(process.hProcess, 99); }
    ::GetExitCodeProcess(process.hProcess, &exit_code);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return exited;
}

void TestCrashReports(const std::wstring& directory) {
    g_case = "no report yet";
    CHECK(!diagnostics::PendingCrashReport(directory).has_value());
    CHECK(!diagnostics::PendingCrashReport(L"").has_value());
    diagnostics::AcknowledgeCrashReports(directory);  // Nothing to acknowledge: no marker either.
    CHECK(Files(directory, L"last-seen.txt").empty());

    std::wstring previous;
    size_t expected_reports = 0;
    static char case_name[64];
    for (const Kind& kind : kKinds) {
        std::snprintf(case_name, sizeof(case_name), "%ls", kind.name);
        g_case = case_name;
        DWORD exit_code = 0;
        CHECK(RunChild(kind.name, directory, exit_code));
        if (exit_code != kind.exit_code) { std::fprintf(stderr, "  exit code 0x%08lX\n", exit_code); }
        CHECK(exit_code == kind.exit_code);

        // One more report; the starting process keeps at most three older ones.
        expected_reports = std::min<size_t>(expected_reports, 3) + 1;
        CHECK(Files(directory, L"test-*.dmp").size() == expected_reports);
        const auto report = diagnostics::PendingCrashReport(directory);
        CHECK(report.has_value());
        if (!report) { continue; }
        CHECK(report->path != previous && report->path.rfind(directory + L"\\test-", 0) == 0);
        previous = report->path;
        const std::string dump = ReadFile(report->path);
        CHECK(dump.size() > 1024 && dump.compare(0, 4, "MDMP") == 0);
        CHECK(dump.size() < 8u * 1024 * 1024);  // Stacks and modules, not the heap.

        diagnostics::AcknowledgeCrashReports(directory);
        CHECK(!diagnostics::PendingCrashReport(directory).has_value());
    }

    g_case = "log of the failing process";
    int fatal_lines = 0, announced = 0, malformed = 0;
    for (const std::wstring& name : Files(directory, L"child*.log")) {
        for (const std::string& line : Lines(ReadFile(directory + L"\\" + name))) {
            if (!WellFormed(line)) { ++malformed; }
            if (line.find("About to fail: ") != std::string::npos) { ++announced; }
            if (line.find("Fatal error 0x") != std::string::npos && line.find("crash report written") != std::string::npos) {
                ++fatal_lines;
            }
        }
    }
    CHECK(malformed == 0);
    CHECK(announced == static_cast<int>(ARRAYSIZE(kKinds)) && fatal_lines == announced);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wcscmp(argv[1], L"--fail") == 0) { return RunFailingProcess(argv[2], argv[3]); }
    const std::wstring directory = MakeDirectory();
    CHECK(!directory.empty());
    if (!directory.empty()) {
        TestLog(directory);
        TestCrashReports(directory);
        RemoveDirectoryWithFiles(directory);
        g_case = "cleanup";
        CHECK(::GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES);
    }
    std::printf("diagnostics: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
