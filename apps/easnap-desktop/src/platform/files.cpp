// Easnap - known folders, file names and directories implementation.
#include "platform/files.h"

#include <cstdio>
#include <memory>
#include <vector>

#include "platform/platform_utils.h"

namespace easnap::platform {

std::optional<std::wstring> KnownFolder(REFKNOWNFOLDERID folder_id) {
    PWSTR raw_path = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(folder_id, KF_FLAG_CREATE, nullptr, &raw_path);
    if (FAILED(hr) || raw_path == nullptr) {
        LogHresult(L"SHGetKnownFolderPath", hr);
        return std::nullopt;
    }
    std::wstring path(raw_path);
    ::CoTaskMemFree(raw_path);
    return path;
}

std::wstring AppDataDirectory() {
    auto local = KnownFolder(FOLDERID_LocalAppData);
    if (!local) {
        return {};
    }
    std::wstring directory = *local + L"\\Easnap";
    EnsureDirectory(directory);
    return directory;
}

std::wstring UniqueFilePath(const std::wstring& directory, const std::wstring& file_name) {
    if (directory.empty() || file_name.empty() ||
        ::GetFileAttributesW(directory.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return {};
    }
    const size_t dot = file_name.find_last_of(L'.');
    const std::wstring stem = dot == std::wstring::npos ? file_name : file_name.substr(0, dot);
    const std::wstring extension = dot == std::wstring::npos ? std::wstring() : file_name.substr(dot);
    std::wstring base = directory;
    if (base.back() != L'\\' && base.back() != L'/') { base += L'\\'; }
    for (int attempt = 1; attempt < 1000; ++attempt) {
        std::wstring candidate = base;
        candidate += stem;
        if (attempt != 1) {
            candidate += L" (";
            candidate += std::to_wstring(attempt);
            candidate += L")";
        }
        candidate += extension;
        if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return {};
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
}

std::wstring TimestampedFileName(const wchar_t* prefix, const wchar_t* extension) {
    SYSTEMTIME now{};
    ::GetLocalTime(&now);
    wchar_t buffer[128]{};
    ::swprintf(buffer, ARRAYSIZE(buffer), L"%s_%04u-%02u-%02u_%02u-%02u-%02u.%s", prefix, now.wYear, now.wMonth,
               now.wDay, now.wHour, now.wMinute, now.wSecond, extension);
    return buffer;
}

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        ::SetLastError(ERROR_PATH_NOT_FOUND);
        return false;
    }
    if (::CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    // The name exists; it serves only if it is a directory and not a file.
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        ::SetLastError(ERROR_DIRECTORY);
        return false;
    }
    return true;
}

bool RemoveDirectoryTree(const std::wstring& path) {
    const auto absolute_path = [](const std::wstring& input) -> std::wstring {
        if (input.empty() || input.find(L'\0') != std::wstring::npos) {
            return {};
        }
        const DWORD capacity = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
        if (capacity == 0) {
            return {};
        }
        std::wstring result(capacity, L'\0');
        const DWORD length = ::GetFullPathNameW(input.c_str(), capacity, result.data(), nullptr);
        if (length == 0 || length >= capacity) {
            return {};
        }
        result.resize(length);
        while (!result.empty() && result.back() == L'\\') {
            result.pop_back();
        }
        return result;
    };
    const std::wstring root = absolute_path(AppDataDirectory());
    const std::wstring target = absolute_path(path);
    // Require a complete path component: Easnap-other is not inside Easnap.
    if (root.empty() || target.size() <= root.size() || target[root.size()] != L'\\' ||
        ::CompareStringOrdinal(target.c_str(), static_cast<int>(root.size()),
                               root.c_str(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL) {
        LogMessage(L"Refusing to delete a directory outside the Easnap data folder");
        return false;
    }

    // Pin every ancestor below the trusted storage root. Refuse reparse points
    // and deny write/delete sharing so a checked directory cannot be replaced
    // or converted to a junction while its descendants are being removed.
    // FILE_LIST_DIRECTORY makes the handle participate in sharing checks;
    // metadata-only FILE_READ_ATTRIBUTES handles do not enforce this exclusion.
    std::vector<UniqueHandle> ancestors;
    for (std::size_t end = root.size(); end != std::wstring::npos;
         end = target.find(L'\\', end + 1)) {
        UniqueHandle handle(::CreateFileW(target.substr(0, end).c_str(),
                                          FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                          FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle.get() == INVALID_HANDLE_VALUE ||
            !::GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }
        ancestors.push_back(std::move(handle));
    }

    // Children are enumerated and opened by path. A checked directory could be
    // turned into a junction between its check and that open (a metadata-only
    // open is exempt from the sharing denial above), so every opened handle
    // must still resolve to a direct child of the directory it was listed in.
    const auto final_path = [](HANDLE handle) -> std::wstring {
        std::wstring result(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = ::GetFinalPathNameByHandleW(
                handle, result.data(), static_cast<DWORD>(result.size()),
                FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
            if (length == 0) {
                return {};
            }
            if (length < result.size()) {
                result.resize(length);
                return result;
            }
            result.resize(static_cast<std::size_t>(length) + 1);
        }
    };
    const auto is_direct_child = [](const std::wstring& child, const std::wstring& parent) {
        return !parent.empty() && child.size() > parent.size() + 1 &&
               child[parent.size()] == L'\\' &&
               child.find(L'\\', parent.size() + 1) == std::wstring::npos &&
               ::CompareStringOrdinal(child.c_str(), static_cast<int>(parent.size()), parent.c_str(),
                                      static_cast<int>(parent.size()), TRUE) == CSTR_EQUAL;
    };

    const auto remove_entry = [&final_path, &is_direct_child](
                                  const auto& self, const std::wstring& entry_path,
                                  const std::wstring& parent_final, bool require_directory) -> bool {
        UniqueHandle handle(::CreateFileW(entry_path.c_str(),
                                          DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ,
                                          nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                          nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        if (handle.get() == INVALID_HANDLE_VALUE ||
            !::GetFileInformationByHandle(handle.get(), &info) ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            (require_directory && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)) {
            return false;
        }
        const std::wstring entry_final = final_path(handle.get());
        if (!is_direct_child(entry_final, parent_final)) {
            LogMessage(L"Refusing to delete an entry that resolved outside its listed directory");
            return false;
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            WIN32_FIND_DATAW entry{};
            const HANDLE find = ::FindFirstFileExW((entry_path + L"\\*").c_str(), FindExInfoBasic,
                                                   &entry, FindExSearchNameMatch, nullptr, 0);
            if (find == INVALID_HANDLE_VALUE) {
                if (::GetLastError() != ERROR_FILE_NOT_FOUND) {
                    return false;
                }
            } else {
                const auto close_find = [](void* value) { ::FindClose(value); };
                std::unique_ptr<void, decltype(close_find)> search(find, close_find);
                do {
                    const std::wstring name(entry.cFileName);
                    if (name != L"." && name != L".." && !self(self, entry_path + L"\\" + name, entry_final, false)) {
                        return false;
                    }
                } while (::FindNextFileW(find, &entry) != FALSE);
                if (::GetLastError() != ERROR_NO_MORE_FILES) {
                    return false;
                }
            }
        }
        if ((info.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes = info.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY;
            if (basic.FileAttributes == 0) {
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            }
            if (!::SetFileInformationByHandle(handle.get(), FileBasicInfo, &basic, sizeof(basic))) {
                return false;
            }
        }
        FILE_DISPOSITION_INFO disposition{TRUE};
        return ::SetFileInformationByHandle(handle.get(), FileDispositionInfo,
                                            &disposition, sizeof(disposition)) != FALSE;
    };
    return remove_entry(remove_entry, target, final_path(ancestors.back().get()), true);
}

}  // namespace easnap::platform
