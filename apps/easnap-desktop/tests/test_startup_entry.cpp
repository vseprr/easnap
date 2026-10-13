// Easnap unit tests - the "start with Windows" entry.
//
// The production functions write to a registry key this test creates under
// HKEY_CURRENT_USER and removes again. The real Run key is only read, to show
// that it is not touched.

#include "platform/startup_entry.h"

#include <cstdio>
#include <string>

namespace {

using namespace easnap::platform;

int g_pass = 0;
int g_fail = 0;

void Check(bool condition, const char* expression, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL %d: %s\n", line, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __LINE__)

std::wstring ReadValue(const std::wstring& key, const wchar_t* name, bool* exists = nullptr) {
    wchar_t data[2048]{};
    DWORD size = sizeof(data) - sizeof(wchar_t);
    const LSTATUS status = ::RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_SZ, nullptr, data, &size);
    if (exists != nullptr) { *exists = status == ERROR_SUCCESS; }
    return status == ERROR_SUCCESS ? std::wstring(data) : std::wstring();
}

}  // namespace

int main() {
    const std::wstring root = L"Software\\EasnapTest-" + std::to_wstring(::GetCurrentProcessId());
    const std::wstring key = root + L"\\Run";
    const wchar_t* const name = L"EasnapTestEntry";
    const std::wstring executable = L"C:\\Program Files\\Easnap Folder\\Easnap.exe";

    bool real_before = false;
    const std::wstring real_value_before = ReadValue(kStartupRunKey, kStartupValueName, &real_before);

    CHECK(StartupCommand(executable) == L"\"C:\\Program Files\\Easnap Folder\\Easnap.exe\" --startup");
    CHECK(!StartupEntryEnabled(executable, key.c_str(), name));  // The key does not exist yet.
    CHECK(SetStartupEntry(false, executable, key.c_str(), name));  // Removing what is not there succeeds.

    CHECK(SetStartupEntry(true, executable, key.c_str(), name));
    CHECK(ReadValue(key, name) == StartupCommand(executable));
    CHECK(StartupEntryEnabled(executable, key.c_str(), name));
    CHECK(StartupEntryEnabled(L"c:\\program files\\easnap folder\\EASNAP.EXE", key.c_str(), name));  // Paths ignore case.
    // An entry written by a copy in another folder is not this copy's entry.
    CHECK(!StartupEntryEnabled(L"D:\\Portable\\Easnap\\bin\\Easnap.exe", key.c_str(), name));
    CHECK(!StartupEntryEnabled(L"", key.c_str(), name));

    // Writing again for another copy replaces the command.
    const std::wstring moved = L"D:\\Portable\\Easnap\\bin\\Easnap.exe";
    CHECK(SetStartupEntry(true, moved, key.c_str(), name));
    CHECK(StartupEntryEnabled(moved, key.c_str(), name) && !StartupEntryEnabled(executable, key.c_str(), name));
    CHECK(!SetStartupEntry(true, L"", key.c_str(), name));  // Nothing to start.
    CHECK(StartupEntryEnabled(moved, key.c_str(), name));

    CHECK(SetStartupEntry(false, moved, key.c_str(), name));
    bool exists = true;
    ReadValue(key, name, &exists);
    CHECK(!exists && !StartupEntryEnabled(moved, key.c_str(), name));
    CHECK(SetStartupEntry(false, moved, key.c_str(), name));

    const std::wstring running = CurrentExecutablePath();
    CHECK(running.size() > 4 && running.find(L"test_startup_entry") != std::wstring::npos);

    CHECK(::RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()) == ERROR_SUCCESS);
    bool real_after = false;
    const std::wstring real_value_after = ReadValue(kStartupRunKey, kStartupValueName, &real_after);
    CHECK(real_before == real_after && real_value_before == real_value_after);

    std::printf("startup entry: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
