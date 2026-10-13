#include "app/recording_completion.h"

#include <shlobj.h>
#include <shellapi.h>
#include <sddl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <new>
#include <utility>
#include <vector>

#include "platform/platform_utils.h"
#include "platform/windows_security.h"

namespace easnap::app {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring NormalizedPath(const std::wstring& path) noexcept {
    try {
        return std::filesystem::weakly_canonical(
                   std::filesystem::path(path))
            .wstring();
    } catch (...) {
        return {};
    }
}

bool SamePath(const std::wstring& left, const std::wstring& right) noexcept {
    const std::wstring normalized_left = NormalizedPath(left);
    const std::wstring normalized_right = NormalizedPath(right);
    return !normalized_left.empty() && !normalized_right.empty() &&
           _wcsicmp(normalized_left.c_str(), normalized_right.c_str()) == 0;
}

bool PathWithinDirectory(const std::wstring& path,
                         const std::wstring& directory) noexcept {
    std::wstring normalized_path = NormalizedPath(path);
    std::wstring normalized_directory = NormalizedPath(directory);
    if (normalized_path.empty() || normalized_directory.empty()) {
        return false;
    }
    while (!normalized_directory.empty() &&
           (normalized_directory.back() == L'\\' ||
            normalized_directory.back() == L'/')) {
        normalized_directory.pop_back();
    }
    if (normalized_path.size() <= normalized_directory.size() ||
        _wcsnicmp(normalized_path.c_str(), normalized_directory.c_str(),
                  normalized_directory.size()) != 0) {
        return false;
    }
    const wchar_t boundary = normalized_path[normalized_directory.size()];
    return boundary == L'\\' || boundary == L'/';
}

bool ExistingNonemptyFile(const std::wstring& path) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard,
                               &attributes) == FALSE ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    const ULARGE_INTEGER size{attributes.nFileSizeLow,
                              attributes.nFileSizeHigh};
    return size.QuadPart != 0;
}

std::wstring UniqueStagingPath(const std::wstring& destination) {
    GUID guid{};
    if (FAILED(::CoCreateGuid(&guid))) {
        return {};
    }
    wchar_t text[40]{};
    if (::StringFromGUID2(guid, text, ARRAYSIZE(text)) == 0) {
        return {};
    }
    std::wstring suffix(text);
    std::erase(suffix, L'{');
    std::erase(suffix, L'}');
    return destination + L".easnap-" + suffix + L".tmp";
}

bool PutClipboardDropFiles(HWND owner, const std::wstring& path) {
    const size_t path_characters = path.size() + 2;
    const size_t bytes = sizeof(DROPFILES) + path_characters * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (memory == nullptr) {
        return false;
    }
    auto* drop = static_cast<DROPFILES*>(::GlobalLock(memory));
    if (drop == nullptr) {
        ::GlobalFree(memory);
        return false;
    }
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    auto* paths = reinterpret_cast<wchar_t*>(
        reinterpret_cast<unsigned char*>(drop) + sizeof(DROPFILES));
    std::copy(path.begin(), path.end(), paths);
    paths[path.size()] = L'\0';
    paths[path.size() + 1] = L'\0';
    ::GlobalUnlock(memory);

    HGLOBAL effect_memory = ::GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effect_memory != nullptr) {
        auto* effect = static_cast<DWORD*>(::GlobalLock(effect_memory));
        if (effect != nullptr) {
            *effect = DROPEFFECT_COPY;
            ::GlobalUnlock(effect_memory);
        } else {
            ::GlobalFree(effect_memory);
            effect_memory = nullptr;
        }
    }

    if (::OpenClipboard(owner) == FALSE) {
        ::GlobalFree(memory);
        if (effect_memory != nullptr) {
            ::GlobalFree(effect_memory);
        }
        return false;
    }
    bool published = false;
    if (::EmptyClipboard() != FALSE &&
        ::SetClipboardData(CF_HDROP, memory) != nullptr) {
        published = true;
        memory = nullptr;
        const UINT effect_format =
            ::RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT);
        if (effect_memory != nullptr && effect_format != 0 &&
            ::SetClipboardData(effect_format, effect_memory) != nullptr) {
            effect_memory = nullptr;
        }
    }
    ::CloseClipboard();
    if (memory != nullptr) {
        ::GlobalFree(memory);
    }
    if (effect_memory != nullptr) {
        ::GlobalFree(effect_memory);
    }
    return published;
}

std::optional<std::vector<std::wstring>> ReadClipboardDropFiles() {
    if (::OpenClipboard(nullptr) == FALSE) {
        return std::nullopt;
    }
    std::vector<std::wstring> paths;
    const HANDLE data = ::GetClipboardData(CF_HDROP);
    if (data != nullptr) {
        const auto drop = reinterpret_cast<HDROP>(data);
        const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        try {
            paths.reserve(count);
            for (UINT index = 0; index < count; ++index) {
                const UINT length =
                    ::DragQueryFileW(drop, index, nullptr, 0);
                std::wstring path(static_cast<size_t>(length) + 1, L'\0');
                if (::DragQueryFileW(drop, index, path.data(), length + 1) != 0) {
                    path.resize(length);
                    paths.push_back(std::move(path));
                }
            }
        } catch (...) {
            ::CloseClipboard();
            return std::nullopt;
        }
    }
    ::CloseClipboard();
    return paths;
}

class WindowsRecordingCompletionBackend final
    : public IRecordingCompletionBackend {
public:
    bool PersistArtifact(const std::wstring& source_path,
                         const std::wstring& destination_path,
                         media::RecordingFormat format,
                         bool replace_existing) override {
        return PersistRecordingFile(source_path, destination_path, format,
                                    replace_existing);
    }

    bool MoveArtifact(const std::wstring& source_path,
                      const std::wstring& destination_path,
                      media::RecordingFormat format) override {
        const ULONGLONG started = ::GetTickCount64();
        const bool moved = ExistingNonemptyFile(source_path) &&
                           IsValidRecordingDestination(source_path, format) &&
                           IsValidRecordingDestination(destination_path, format) &&
                           !SamePath(source_path, destination_path) &&
                           ::MoveFileExW(source_path.c_str(),
                                         destination_path.c_str(),
                                         MOVEFILE_WRITE_THROUGH) != FALSE;
        platform::LogMessage(
            L"[CompletionTiming] publication-move=%llu ms result=%s",
            ::GetTickCount64() - started, moved ? L"success" : L"failed");
        return moved;
    }

    bool ApplyClipboardSecurity(const std::wstring& path) override {
        return ApplyClipboardPublicationSecurity(path);
    }

    [[nodiscard]] bool IsNonemptyArtifact(
        const std::wstring& path, media::RecordingFormat format) const override {
        return IsValidRecordingDestination(path, format) &&
               ExistingNonemptyFile(path);
    }

    bool PutFileOnClipboard(HWND owner,
                            const std::wstring& path) override {
        return PutClipboardDropFiles(owner, path);
    }

    [[nodiscard]] std::optional<std::vector<std::wstring>>
    ClipboardFilePaths() const override {
        return ReadClipboardDropFiles();
    }
};

}  // namespace

bool TrimArtifact::Matches(media::MediaTime requested_start,
                           media::MediaTime requested_end) const noexcept {
    return !path.empty() && start == requested_start && end == requested_end &&
           end > start;
}

void TrimArtifact::Clear() noexcept {
    path.clear();
    start = media::MediaTime::zero();
    end = media::MediaTime::zero();
}

TrimCloseAction PlanTrimClose(bool discard_confirmed,
                              bool export_running) noexcept {
    if (!discard_confirmed) {
        return TrimCloseAction::KeepOpen;
    }
    return export_running ? TrimCloseAction::CancelExportThenDiscard
                          : TrimCloseAction::DiscardNow;
}

std::unique_ptr<IRecordingCompletionBackend>
CreateRecordingCompletionBackend() {
    return std::make_unique<WindowsRecordingCompletionBackend>();
}

bool IsValidRecordingDestination(const std::wstring& path,
                                 media::RecordingFormat format) noexcept {
    try {
        const std::filesystem::path candidate(path);
        if (!candidate.is_absolute() || !candidate.has_filename()) {
            return false;
        }
        std::wstring extension = candidate.extension().wstring();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](wchar_t value) {
                           return static_cast<wchar_t>(std::towlower(value));
                       });
        const wchar_t* expected =
            format == media::RecordingFormat::Gif ? L".gif" : L".mp4";
        return (format == media::RecordingFormat::Mp4 ||
                format == media::RecordingFormat::Gif) &&
               extension == expected;
    } catch (...) {
        return false;
    }
}

bool PersistRecordingFile(const std::wstring& source_path,
                          const std::wstring& destination_path,
                          media::RecordingFormat format,
                          bool replace_existing) {
    if (!IsValidRecordingDestination(source_path, format) ||
        !IsValidRecordingDestination(destination_path, format) ||
        SamePath(source_path, destination_path) ||
        !ExistingNonemptyFile(source_path)) {
        return false;
    }
    const DWORD destination_attributes =
        ::GetFileAttributesW(destination_path.c_str());
    if (destination_attributes != INVALID_FILE_ATTRIBUTES &&
        (!replace_existing ||
         (destination_attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        return false;
    }

    const ULONGLONG started = ::GetTickCount64();
    const std::wstring staging_path = UniqueStagingPath(destination_path);
    if (staging_path.empty() ||
        ::CopyFileW(source_path.c_str(), staging_path.c_str(), TRUE) == FALSE) {
        platform::LogMessage(
            L"[CompletionTiming] publication-copy failed after %llu ms",
            ::GetTickCount64() - started);
        return false;
    }
    const ULONGLONG copied = ::GetTickCount64();
    const DWORD flags = MOVEFILE_WRITE_THROUGH |
                        (replace_existing ? MOVEFILE_REPLACE_EXISTING : 0);
    if (::MoveFileExW(staging_path.c_str(), destination_path.c_str(), flags) ==
        FALSE) {
        ::DeleteFileW(staging_path.c_str());
        platform::LogMessage(
            L"[CompletionTiming] publication-finalize failed after %llu ms",
            ::GetTickCount64() - started);
        return false;
    }
    platform::LogMessage(
        L"[CompletionTiming] publication-copy=%llu ms finalize=%llu ms total=%llu ms",
        copied - started, ::GetTickCount64() - copied,
        ::GetTickCount64() - started);
    return true;
}

bool ApplyClipboardPublicationSecurity(const std::wstring& path) {
    return platform::ApplyCurrentUserSystemSecurity(path);
}

RecordingCompletionController::RecordingCompletionController(
    media::ITemporarySessionStore& session_store,
    std::unique_ptr<IRecordingCompletionBackend> backend)
    : session_store_(session_store), backend_(std::move(backend)) {}

RecordingCompletionController::~RecordingCompletionController() { Shutdown(); }

bool RecordingCompletionController::Save(
    const std::wstring& source_path, const std::wstring& destination_path,
    media::RecordingFormat format, bool replace_existing) {
    return backend_ != nullptr &&
           IsAllowedSaveDestination(destination_path, format,
                                    replace_existing) &&
           backend_->PersistArtifact(source_path, destination_path, format,
                                     replace_existing);
}

bool RecordingCompletionController::IsAllowedSaveDestination(
    const std::wstring& destination_path, media::RecordingFormat format,
    bool replace_existing) const {
    if (!IsValidRecordingDestination(destination_path, format) ||
        PathWithinDirectory(destination_path, session_store_.root_directory())) {
        return false;
    }
    const DWORD attributes = ::GetFileAttributesW(destination_path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (!replace_existing || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) {
        return false;
    }
    return true;
}

std::optional<std::wstring>
RecordingCompletionController::PrepareClipboardDestination(
    media::RecordingFormat format) {
    if (backend_ == nullptr ||
        (format != media::RecordingFormat::Mp4 &&
         format != media::RecordingFormat::Gif)) {
        return std::nullopt;
    }
    AbandonPreparedClipboardArtifact();
    if (prepared_clipboard_session_.has_value()) {
        return std::nullopt;
    }
    const auto session = session_store_.CreateSession();
    if (!session.has_value() || !session->valid() ||
        !backend_->ApplyClipboardSecurity(session->directory)) {
        if (session.has_value()) {
            session_store_.DeleteSession(*session);
        }
        return std::nullopt;
    }
    prepared_clipboard_session_ = *session;
    prepared_clipboard_path_ = session->publication_path(format);
    return prepared_clipboard_path_;
}

std::optional<std::wstring>
RecordingCompletionController::StageClipboardArtifact(
    const std::wstring& source_path, media::RecordingFormat format,
    bool allow_move) {
    const auto destination = PrepareClipboardDestination(format);
    if (!destination.has_value()) {
        return std::nullopt;
    }
    const bool staged =
        (allow_move && backend_->MoveArtifact(source_path, *destination, format)) ||
        backend_->PersistArtifact(source_path, *destination, format, false);
    if (!staged) {
        AbandonPreparedClipboardArtifact();
        return std::nullopt;
    }
    return destination;
}

bool RecordingCompletionController::PublishClipboardArtifact(
    HWND owner, const std::wstring& path, media::RecordingFormat format) {
    if (!IsPreparedClipboardArtifact(path) || backend_ == nullptr ||
        !backend_->IsNonemptyArtifact(path, format) ||
        !backend_->ApplyClipboardSecurity(path) ||
        !backend_->PutFileOnClipboard(owner, path)) {
        OnClipboardChanged();
        return false;
    }
    if (clipboard_session_.has_value()) {
        session_store_.DeleteSession(*clipboard_session_);
    }
    clipboard_session_ = prepared_clipboard_session_;
    clipboard_path_ = prepared_clipboard_path_;
    prepared_clipboard_session_.reset();
    prepared_clipboard_path_.clear();
    return true;
}

bool RecordingCompletionController::IsPreparedClipboardArtifact(
    const std::wstring& path) const noexcept {
    return prepared_clipboard_session_.has_value() &&
           SamePath(prepared_clipboard_path_, path);
}

void RecordingCompletionController::AbandonPreparedClipboardArtifact() {
    if (prepared_clipboard_session_.has_value() &&
        session_store_.DeleteSession(*prepared_clipboard_session_)) {
        prepared_clipboard_session_.reset();
        prepared_clipboard_path_.clear();
    }
}

bool RecordingCompletionController::Discard(
    const media::TemporarySession& session) {
    return session_store_.DeleteSession(session);
}

int RecordingCompletionController::CleanupStaleSessions() {
    if (backend_ == nullptr) {
        return 0;
    }
    const auto paths = backend_->ClipboardFilePaths();
    if (!paths.has_value()) {
        return 0;
    }
    std::vector<std::wstring> protected_directories;
    protected_directories.reserve(paths->size());
    for (const std::wstring& path : *paths) {
        const std::wstring directory = platform::DirectoryOf(path);
        if (!directory.empty()) {
            protected_directories.push_back(directory);
        }
    }
    return session_store_.CleanupStaleSessions(protected_directories);
}

std::optional<bool> RecordingCompletionController::ClipboardReferences(
    const std::wstring& path) const {
    if (backend_ == nullptr) {
        return std::nullopt;
    }
    const auto paths = backend_->ClipboardFilePaths();
    if (!paths.has_value()) {
        return std::nullopt;
    }
    return std::any_of(paths->begin(), paths->end(),
                       [&path](const std::wstring& candidate) {
                           return SamePath(path, candidate);
                       });
}

void RecordingCompletionController::OnClipboardChanged() {
    if (!clipboard_session_.has_value()) {
        return;
    }
    const auto referenced =
        ClipboardReferences(clipboard_path_);
    if (referenced.has_value() && !*referenced &&
        session_store_.DeleteSession(*clipboard_session_)) {
        clipboard_session_.reset();
        clipboard_path_.clear();
    }
}

void RecordingCompletionController::Shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    AbandonPreparedClipboardArtifact();
    if (!clipboard_session_.has_value()) {
        return;
    }
    const auto referenced =
        ClipboardReferences(clipboard_path_);
    if (referenced.has_value() && !*referenced) {
        if (session_store_.DeleteSession(*clipboard_session_)) {
            clipboard_session_.reset();
            clipboard_path_.clear();
        }
        return;
    }
    session_store_.ReleaseSession(*clipboard_session_);
    clipboard_session_.reset();
    clipboard_path_.clear();
}

std::optional<std::wstring> ShowSaveRecordingDialog(
    HWND owner, media::RecordingFormat format,
    const std::wstring& suggested_file_name,
    const std::wstring& initial_directory,
    const SaveRecordingDialogText& text) {
    ComPtr<IFileSaveDialog> dialog;
    HRESULT result = ::CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        return std::nullopt;
    }
    // Built for each dialog: the type name follows the active language. The
    // dialog copies the filter.
    const COMDLG_FILTERSPEC mp4_filters[] = {
        {text.mp4_type, L"*.mp4"},
    };
    const COMDLG_FILTERSPEC gif_filters[] = {
        {text.gif_type, L"*.gif"},
    };
    const COMDLG_FILTERSPEC* filters =
        format == media::RecordingFormat::Gif ? gif_filters : mp4_filters;
    dialog->SetFileTypes(1, filters);
    dialog->SetFileTypeIndex(1);
    dialog->SetDefaultExtension(
        format == media::RecordingFormat::Gif ? L"gif" : L"mp4");
    dialog->SetTitle(text.title);
    dialog->SetFileName(suggested_file_name.c_str());

    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST |
                           FOS_FORCEFILESYSTEM | FOS_NOREADONLYRETURN);
    }
    if (!initial_directory.empty()) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(
                initial_directory.c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetFolder(folder.Get());  // The folder asked for, not the one used last.
        }
    } else if (const auto videos = platform::KnownFolder(FOLDERID_Videos)) {
        ComPtr<IShellItem> folder;
        if (SUCCEEDED(::SHCreateItemFromParsingName(
                videos->c_str(), nullptr, IID_PPV_ARGS(&folder)))) {
            dialog->SetDefaultFolder(folder.Get());
        }
    }

    {
        const platform::ModalLoopScope modal;
        result = dialog->Show(owner);
    }
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED) || FAILED(result)) {
        return std::nullopt;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) {
        return std::nullopt;
    }
    PWSTR raw_path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path)) ||
        raw_path == nullptr) {
        return std::nullopt;
    }
    std::wstring path(raw_path);
    ::CoTaskMemFree(raw_path);
    if (!IsValidRecordingDestination(path, format)) {
        return std::nullopt;
    }
    return path;
}

}  // namespace easnap::app
