// Easnap - "start with Windows" entry of the current user.
//
// One value under the user's Run key. Nothing is written elsewhere and no
// administrator rights are involved. The key path is a parameter so that tests
// can work on a key of their own.
#pragma once

#include <windows.h>

#include <string>

namespace easnap::platform {

inline constexpr wchar_t kStartupRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr wchar_t kStartupValueName[] = L"Easnap";
/// Appended to the command, so a start at sign-in can stay quiet.
inline constexpr wchar_t kStartupArgument[] = L"--startup";

/// Full path of the running executable.
[[nodiscard]] std::wstring CurrentExecutablePath();

/// The command the entry holds for `executable`: the quoted path and the
/// startup argument.
[[nodiscard]] std::wstring StartupCommand(const std::wstring& executable);

/// True when the entry exists and starts exactly `executable`. An entry left
/// by a copy in another folder does not count.
[[nodiscard]] bool StartupEntryEnabled(const std::wstring& executable, const wchar_t* key_path = kStartupRunKey,
                                       const wchar_t* value_name = kStartupValueName);

/// Writes or removes the entry. Removing an entry that does not exist succeeds.
bool SetStartupEntry(bool enabled, const std::wstring& executable, const wchar_t* key_path = kStartupRunKey,
                     const wchar_t* value_name = kStartupValueName);

}  // namespace easnap::platform
