// Easnap - "start with Windows" entry of the current user.
#include "platform/startup_entry.h"

#include <vector>

#include "platform/platform_utils.h"

namespace easnap::platform {

std::wstring CurrentExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return std::wstring(buffer.data(), length);
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring StartupCommand(const std::wstring& executable) {
    return L"\"" + executable + L"\" " + kStartupArgument;
}

bool StartupEntryEnabled(const std::wstring& executable, const wchar_t* key_path, const wchar_t* value_name) {
    if (executable.empty()) {
        return false;
    }
    wchar_t data[2048]{};
    DWORD size = sizeof(data) - sizeof(wchar_t);
    if (::RegGetValueW(HKEY_CURRENT_USER, key_path, value_name, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS) {
        return false;
    }
    return ::_wcsicmp(data, StartupCommand(executable).c_str()) == 0;
}

bool SetStartupEntry(bool enabled, const std::wstring& executable, const wchar_t* key_path, const wchar_t* value_name) {
    HKEY key = nullptr;
    LSTATUS status = ::RegCreateKeyExW(HKEY_CURRENT_USER, key_path, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (status != ERROR_SUCCESS) {
        LogMessage(L"The startup key could not be opened (error %ld)", status);
        return false;
    }
    if (enabled) {
        const std::wstring command = StartupCommand(executable);
        status = executable.empty()
                     ? ERROR_INVALID_PARAMETER
                     : ::RegSetValueExW(key, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                        static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = ::RegDeleteValueW(key, value_name);
        if (status == ERROR_FILE_NOT_FOUND) {
            status = ERROR_SUCCESS;
        }
    }
    ::RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        LogMessage(L"The startup entry could not be %ls (error %ld)", enabled ? L"written" : L"removed", status);
    }
    return status == ERROR_SUCCESS;
}

}  // namespace easnap::platform
