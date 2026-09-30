// Easnap - small Windows security helpers shared by private and publication
// temporary-media storage.
#pragma once

#include <sddl.h>

#include <optional>
#include <string>
#include <vector>

#include "platform/platform_utils.h"

namespace easnap::platform {

inline std::optional<std::wstring> CurrentProcessUserSidString() {
    HANDLE raw_token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) ==
        FALSE) {
        return std::nullopt;
    }
    const UniqueHandle token(raw_token);
    DWORD required = 0;
    ::GetTokenInformation(token.get(), TokenUser, nullptr, 0, &required);
    if (required == 0 || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::nullopt;
    }
    std::vector<unsigned char> buffer(required);
    if (::GetTokenInformation(token.get(), TokenUser, buffer.data(), required,
                              &required) == FALSE) {
        return std::nullopt;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR raw_sid = nullptr;
    if (::ConvertSidToStringSidW(user->User.Sid, &raw_sid) == FALSE ||
        raw_sid == nullptr) {
        return std::nullopt;
    }
    std::wstring sid(raw_sid);
    ::LocalFree(raw_sid);
    return sid;
}

inline PSECURITY_DESCRIPTOR CreateCurrentUserSystemSecurityDescriptor() {
    const auto user_sid = CurrentProcessUserSidString();
    if (!user_sid.has_value()) {
        return nullptr;
    }
    const std::wstring sddl = L"D:P(A;OICI;GA;;;" + *user_sid +
                              L")(A;OICI;GA;;;SY)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE) {
        return nullptr;
    }
    return descriptor;
}

inline bool ApplyCurrentUserSystemSecurity(const std::wstring& path) {
    if (path.empty() ||
        ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    PSECURITY_DESCRIPTOR descriptor =
        CreateCurrentUserSystemSecurityDescriptor();
    if (descriptor == nullptr) {
        return false;
    }
    const BOOL applied = ::SetFileSecurityW(
        path.c_str(),
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        descriptor);
    ::LocalFree(descriptor);
    return applied != FALSE;
}

}  // namespace easnap::platform
