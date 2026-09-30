// Easnap - temporary session storage.
//
// Long recordings are streamed to disk instead of RAM, so Easnap needs a
// predictable, private and self-cleaning scratch area:
//
//   %LOCALAPPDATA%\Easnap\temp\{random-guid}
//
// * The directory name is a generated GUID - never a user-supplied string, so
//   no user input can influence the path.
// * The DACL is inherit-protected and only grants the current user and SYSTEM,
//   which keeps captured media away from other accounts on the machine.
// * Sessions are removed on normal completion, on discard and on startup (which
//   collects anything left behind by a crash or a power loss).
#pragma once

#include <sddl.h>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "platform/platform_utils.h"
#include "platform/windows_security.h"
#include "media/media_interfaces.h"

namespace easnap::media {

struct TemporarySession {
    std::wstring id;         ///< Random identifier (GUID form, no braces).
    std::wstring directory;  ///< Absolute path of the session directory.

    [[nodiscard]] bool valid() const noexcept { return !directory.empty(); }
    [[nodiscard]] std::wstring recording_path() const {
        return valid() ? directory + L"\\recording.mp4" : std::wstring{};
    }
    [[nodiscard]] std::wstring publication_path(RecordingFormat format) const {
        if (!valid()) {
            return {};
        }
        return directory + (format == RecordingFormat::Gif
                                ? L"\\recording.gif"
                                : L"\\recording.mp4");
    }
};

class ITemporarySessionStore {
public:
    virtual ~ITemporarySessionStore() = default;

    /// Creates a fresh, access-restricted session directory.
    virtual std::optional<TemporarySession> CreateSession() = 0;
    /// Lists every session directory currently present on disk.
    [[nodiscard]] virtual std::vector<std::wstring> EnumerateSessions() const = 0;
    /// Deletes one session and everything inside it.
    virtual bool DeleteSession(const TemporarySession& session) = 0;
    /// Stops owning a session without deleting it. Used only when the Windows
    /// clipboard still references media during process shutdown.
    virtual bool ReleaseSession(const TemporarySession& session) = 0;
    /// Removes sessions that are not owned by this process instance and are not
    /// explicitly protected by an external lifetime owner.
    /// Returns the number of directories deleted.
    virtual int CleanupStaleSessions(
        const std::vector<std::wstring>& protected_directories = {}) = 0;
    [[nodiscard]] virtual const std::wstring& root_directory() const = 0;
};

/// Local-disk implementation. Header-only: it is a thin wrapper over the
/// platform helpers and has no state worth hiding in a translation unit.
class LocalTemporarySessionStore final : public ITemporarySessionStore {
public:
    LocalTemporarySessionStore() {
        const std::wstring app_data = platform::AppDataDirectory();
        if (!app_data.empty()) {
            root_ = app_data + L"\\temp";
            platform::EnsureDirectory(root_);
        }
    }

    ~LocalTemporarySessionStore() override {
        // Normal shutdown: nothing owned by this instance may survive.
        for (OwnedSession& session : owned_) {
            session.CloseLease();
            platform::RemoveDirectoryTree(session.directory);
        }
    }

    std::optional<TemporarySession> CreateSession() override {
        if (root_.empty()) {
            return std::nullopt;
        }
        const std::wstring id = GenerateSessionId();
        if (id.empty()) {
            return std::nullopt;
        }

        TemporarySession session;
        session.id = id;
        session.directory = root_ + L"\\" + id;

        SECURITY_ATTRIBUTES attributes{};
        // TokenDefaultOwner is not required to equal TokenUser (notably for
        // elevated administrator tokens), so OWNER RIGHTS is not a stable
        // same-user access contract. Grant the concrete process-user SID and
        // fail closed if that protected descriptor cannot be constructed.
        PSECURITY_DESCRIPTOR descriptor =
            platform::CreateCurrentUserSystemSecurityDescriptor();
        if (descriptor == nullptr) {
            platform::LogMessage(
                L"Failed to construct temporary-session security");
            return std::nullopt;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = descriptor;
        attributes.bInheritHandle = FALSE;

        const BOOL created =
            ::CreateDirectoryW(session.directory.c_str(), &attributes);
        ::LocalFree(descriptor);
        if (created == FALSE) {
            platform::LogMessage(L"Failed to create a temporary session directory (error %lu)", ::GetLastError());
            return std::nullopt;
        }

        // Keep an exclusive on-disk lease for the entire recording/trim/
        // clipboard lifetime. A second Easnap process (or test) must not call
        // an active session "stale" merely because its own owned_ is empty.
        const std::wstring lease_path = session.directory + L"\\.session-lock";
        const HANDLE lease = ::CreateFileW(
            lease_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr);
        if (lease == INVALID_HANDLE_VALUE) {
            platform::LogMessage(L"Failed to lease a temporary session (error %lu)",
                                 ::GetLastError());
            platform::RemoveDirectoryTree(session.directory);
            return std::nullopt;
        }
        try {
            owned_.emplace_back(session.directory, lease);
        } catch (...) {
            ::CloseHandle(lease);
            platform::RemoveDirectoryTree(session.directory);
            return std::nullopt;
        }
        platform::LogMessage(L"Temporary session created");
        return session;
    }

    [[nodiscard]] std::vector<std::wstring> EnumerateSessions() const override {
        std::vector<std::wstring> sessions;
        if (root_.empty()) {
            return sessions;
        }
        WIN32_FIND_DATAW entry{};
        const std::wstring pattern = root_ + L"\\*";
        HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &entry, FindExSearchLimitToDirectories,
                                         nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) {
            return sessions;
        }
        do {
            const std::wstring name(entry.cFileName);
            if (name == L"." || name == L".." || (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                continue;
            }
            sessions.push_back(root_ + L"\\" + name);
        } while (::FindNextFileW(find, &entry) != FALSE);
        ::FindClose(find);
        return sessions;
    }

    bool DeleteSession(const TemporarySession& session) override {
        auto owned = std::find_if(owned_.begin(), owned_.end(),
            [&session](const OwnedSession& candidate) {
                return candidate.directory == session.directory;
            });
        if (!session.valid() || owned == owned_.end()) {
            return false;
        }
        owned->CloseLease();
        if (!platform::RemoveDirectoryTree(session.directory)) {
            owned->lease = ::CreateFileW(
                (session.directory + L"\\.session-lock").c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_HIDDEN, nullptr);
            return false;
        }
        owned_.erase(owned);
        return true;
    }

    bool ReleaseSession(const TemporarySession& session) override {
        if (!session.valid()) {
            return false;
        }
        const auto before = owned_.size();
        std::erase_if(owned_, [&session](const OwnedSession& candidate) {
            return candidate.directory == session.directory;
        });
        return owned_.size() != before;
    }

    int CleanupStaleSessions(
        const std::vector<std::wstring>& protected_directories = {}) override {
        int removed = 0;
        for (const std::wstring& directory : EnumerateSessions()) {
            const bool owned = std::any_of(owned_.begin(), owned_.end(),
                [&directory](const OwnedSession& candidate) {
                    return candidate.directory == directory;
                });
            const bool protected_directory = std::any_of(
                protected_directories.begin(), protected_directories.end(),
                [&directory](const std::wstring& candidate) {
                    return _wcsicmp(directory.c_str(), candidate.c_str()) == 0;
                });
            if (owned || protected_directory) {
                continue;
            }
            // Fail closed for unknown/legacy folders, reparse points and live
            // leases. The missing-marker case also covers the short interval
            // between creating a directory and opening its first lease.
            const DWORD attributes = ::GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES ||
                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                continue;
            }
            const HANDLE lease = ::CreateFileW(
                (directory + L"\\.session-lock").c_str(),
                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (lease == INVALID_HANDLE_VALUE) {
                continue;
            }
            ::CloseHandle(lease);
            if (platform::RemoveDirectoryTree(directory)) {
                ++removed;
            }
        }
        if (removed > 0) {
            platform::LogMessage(L"Removed %d stale temporary session(s)", removed);
        }
        return removed;
    }

    [[nodiscard]] const std::wstring& root_directory() const override { return root_; }

private:
    struct OwnedSession {
        std::wstring directory;
        HANDLE lease{INVALID_HANDLE_VALUE};

        OwnedSession(std::wstring path, HANDLE handle)
            : directory(std::move(path)), lease(handle) {}
        ~OwnedSession() { CloseLease(); }
        OwnedSession(const OwnedSession&) = delete;
        OwnedSession& operator=(const OwnedSession&) = delete;
        OwnedSession(OwnedSession&& other) noexcept
            : directory(std::move(other.directory)),
              lease(std::exchange(other.lease, INVALID_HANDLE_VALUE)) {}
        OwnedSession& operator=(OwnedSession&& other) noexcept {
            if (this != &other) {
                CloseLease();
                directory = std::move(other.directory);
                lease = std::exchange(other.lease, INVALID_HANDLE_VALUE);
            }
            return *this;
        }
        void CloseLease() noexcept {
            if (lease != INVALID_HANDLE_VALUE) {
                ::CloseHandle(std::exchange(lease, INVALID_HANDLE_VALUE));
            }
        }
    };

    static std::wstring GenerateSessionId() {
        GUID guid{};
        if (FAILED(::CoCreateGuid(&guid))) {
            return {};
        }
        wchar_t buffer[40]{};
        if (::StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0) {
            return {};
        }
        std::wstring id(buffer);
        // Strip the surrounding braces so the folder name stays path friendly.
        if (id.size() >= 2 && id.front() == L'{' && id.back() == L'}') {
            id = id.substr(1, id.size() - 2);
        }
        return id;
    }

    std::wstring root_;
    std::vector<OwnedSession> owned_;
};

inline std::unique_ptr<ITemporarySessionStore> CreateTemporarySessionStore() {
    return std::make_unique<LocalTemporarySessionStore>();
}

}  // namespace easnap::media
