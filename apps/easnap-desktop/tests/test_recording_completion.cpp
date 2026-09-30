#include <windows.h>
#include <aclapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/recording_completion.h"
#include "app/session_transitions.h"

namespace {

int g_passed = 0;
int g_failed = 0;

#define CHECK(expression)                                                        \
    do {                                                                         \
        if (expression) {                                                        \
            ++g_passed;                                                          \
        } else {                                                                 \
            ++g_failed;                                                          \
            std::cerr << "FAIL line " << __LINE__ << ": " #expression << '\n'; \
        }                                                                        \
    } while (false)

using easnap::app::IRecordingCompletionBackend;
using easnap::media::RecordingFormat;
using easnap::media::TemporarySession;

class FakeSessionStore final : public easnap::media::ITemporarySessionStore {
public:
    std::optional<TemporarySession> CreateSession() override {
        TemporarySession session;
        session.id = L"session-" + std::to_wstring(++created_count);
        session.directory = L"C:\\fake\\easnap\\" + session.id;
        owned.push_back(session.directory);
        return session;
    }

    [[nodiscard]] std::vector<std::wstring> EnumerateSessions() const override {
        return owned;
    }

    bool DeleteSession(const TemporarySession& session) override {
        ++delete_count;
        deleted.push_back(session.directory);
        std::erase(owned, session.directory);
        return delete_succeeds;
    }

    bool ReleaseSession(const TemporarySession& session) override {
        ++release_count;
        released.push_back(session.directory);
        std::erase(owned, session.directory);
        return true;
    }

    int CleanupStaleSessions(
        const std::vector<std::wstring>& protected_directories = {}) override {
        ++cleanup_count;
        last_protected = protected_directories;
        return cleanup_result;
    }

    [[nodiscard]] const std::wstring& root_directory() const override {
        return root;
    }

    int created_count{0};
    int delete_count{0};
    int release_count{0};
    int cleanup_count{0};
    int cleanup_result{2};
    bool delete_succeeds{true};
    std::wstring root{L"C:\\fake\\easnap"};
    std::vector<std::wstring> owned;
    std::vector<std::wstring> deleted;
    std::vector<std::wstring> released;
    std::vector<std::wstring> last_protected;
};

class FakeBackend final : public IRecordingCompletionBackend {
public:
    bool PersistArtifact(const std::wstring& source_path,
                         const std::wstring& destination_path,
                         RecordingFormat format,
                         bool replace_existing) override {
        ++persist_count;
        last_source = source_path;
        last_destination = destination_path;
        last_replace = replace_existing;
        last_format = format;
        return persist_succeeds;
    }

    bool MoveArtifact(const std::wstring& source_path,
                      const std::wstring& destination_path,
                      RecordingFormat format) override {
        ++move_count;
        last_source = source_path;
        last_destination = destination_path;
        last_format = format;
        return move_succeeds;
    }

    bool ApplyClipboardSecurity(const std::wstring& path) override {
        ++security_count;
        secured_paths.push_back(path);
        return security_succeeds;
    }

    [[nodiscard]] bool IsNonemptyArtifact(
        const std::wstring&, RecordingFormat format) const override {
        last_checked_format = format;
        return nonempty_mp4;
    }

    bool PutFileOnClipboard(HWND, const std::wstring& path) override {
        ++clipboard_publish_count;
        if (clipboard_succeeds) {
            clipboard_paths = std::vector<std::wstring>{path};
        }
        return clipboard_succeeds;
    }

    [[nodiscard]] std::optional<std::vector<std::wstring>>
    ClipboardFilePaths() const override {
        return clipboard_paths;
    }

    bool persist_succeeds{true};
    bool move_succeeds{true};
    bool security_succeeds{true};
    bool nonempty_mp4{true};
    bool clipboard_succeeds{true};
    bool last_replace{false};
    int persist_count{0};
    int move_count{0};
    int security_count{0};
    int clipboard_publish_count{0};
    std::wstring last_source;
    std::wstring last_destination;
    RecordingFormat last_format{RecordingFormat::Mp4};
    mutable RecordingFormat last_checked_format{RecordingFormat::Mp4};
    std::vector<std::wstring> secured_paths;
    std::optional<std::vector<std::wstring>> clipboard_paths{
        std::vector<std::wstring>{}};
};

std::filesystem::path MakeTestDirectory() {
    wchar_t temporary[MAX_PATH]{};
    ::GetTempPathW(ARRAYSIZE(temporary), temporary);
    const auto unique = L"EasnapCompletion-" +
                        std::to_wstring(::GetCurrentProcessId()) + L"-" +
                        std::to_wstring(::GetTickCount64());
    const std::filesystem::path directory =
        std::filesystem::path(temporary) / unique;
    std::filesystem::create_directories(directory);
    return directory;
}

void WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::string ReadBytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>());
}

struct AclSummary final {
    bool readable{false};
    bool protected_dacl{false};
    bool owner_is_current_user{false};
    bool current_user_allowed{false};
    bool system_allowed{false};
    bool everyone_allowed{false};
};

std::vector<unsigned char> CurrentUserSid() {
    HANDLE raw_token = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw_token) ==
        FALSE) {
        return {};
    }
    DWORD required = 0;
    ::GetTokenInformation(raw_token, TokenUser, nullptr, 0, &required);
    std::vector<unsigned char> token_buffer(required);
    if (required == 0 ||
        ::GetTokenInformation(raw_token, TokenUser, token_buffer.data(),
                              required, &required) == FALSE) {
        ::CloseHandle(raw_token);
        return {};
    }
    const auto* user =
        reinterpret_cast<const TOKEN_USER*>(token_buffer.data());
    const DWORD sid_size = ::GetLengthSid(user->User.Sid);
    std::vector<unsigned char> sid(sid_size);
    if (::CopySid(sid_size, sid.data(), user->User.Sid) == FALSE) {
        sid.clear();
    }
    ::CloseHandle(raw_token);
    return sid;
}

std::vector<unsigned char> WellKnownSid(WELL_KNOWN_SID_TYPE type) {
    std::vector<unsigned char> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    if (::CreateWellKnownSid(type, nullptr, sid.data(), &size) == FALSE) {
        return {};
    }
    sid.resize(size);
    return sid;
}

PSID SidPointer(const std::vector<unsigned char>& sid) {
    return sid.empty() ? nullptr
                       : static_cast<PSID>(
                             const_cast<unsigned char*>(sid.data()));
}

AclSummary InspectAcl(const std::wstring& path) {
    AclSummary summary;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PSID owner = nullptr;
    PACL dacl = nullptr;
    if (::GetNamedSecurityInfoW(
            const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS) {
        return summary;
    }
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    summary.protected_dacl =
        ::GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE &&
        (control & SE_DACL_PROTECTED) != 0;
    const auto current_user = CurrentUserSid();
    const auto system = WellKnownSid(WinLocalSystemSid);
    const auto everyone = WellKnownSid(WinWorldSid);
    summary.owner_is_current_user =
        owner != nullptr && !current_user.empty() &&
        ::EqualSid(owner, SidPointer(current_user)) != FALSE;
    if (dacl != nullptr) {
        for (DWORD index = 0; index < dacl->AceCount; ++index) {
            void* raw_ace = nullptr;
            if (::GetAce(dacl, index, &raw_ace) == FALSE || raw_ace == nullptr) {
                continue;
            }
            const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                continue;
            }
            const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
            PSID sid = const_cast<DWORD*>(&ace->SidStart);
            const bool has_access = ace->Mask != 0;
            summary.current_user_allowed =
                summary.current_user_allowed ||
                (has_access && !current_user.empty() &&
                 ::EqualSid(sid, SidPointer(current_user)) != FALSE);
            summary.system_allowed =
                summary.system_allowed ||
                (has_access && !system.empty() &&
                 ::EqualSid(sid, SidPointer(system)) != FALSE);
            summary.everyone_allowed =
                summary.everyone_allowed ||
                (has_access && !everyone.empty() &&
                 ::EqualSid(sid, SidPointer(everyone)) != FALSE);
        }
    }
    ::LocalFree(descriptor);

    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE |
                                    FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    summary.readable = file != INVALID_HANDLE_VALUE;
    if (file != INVALID_HANDLE_VALUE) {
        ::CloseHandle(file);
    }
    return summary;
}

void TestLiveTemporarySessionLease() {
    auto owner = easnap::media::CreateTemporarySessionStore();
    auto collector = easnap::media::CreateTemporarySessionStore();
    const auto session = owner->CreateSession();
    CHECK(session.has_value());
    if (!session) {
        return;
    }
    WriteBytes(session->recording_path(), "live-recording");
    // Exercise real Windows sharing semantics, but never collect unrelated
    // user sessions while this regression runs.
    auto protected_directories = collector->EnumerateSessions();
    std::erase(protected_directories, session->directory);
    CHECK(collector->CleanupStaleSessions(protected_directories) == 0);
    CHECK(std::filesystem::exists(session->recording_path()));
    CHECK(!collector->DeleteSession(*session));
    CHECK(std::filesystem::exists(session->recording_path()));
    CHECK(owner->ReleaseSession(*session));
    CHECK(collector->CleanupStaleSessions(protected_directories) == 1);
    CHECK(!std::filesystem::exists(session->directory));
}

void TestClipboardPublicationAcl() {
    auto store = easnap::media::CreateTemporarySessionStore();
    CHECK(store != nullptr);
    const auto private_session =
        store != nullptr ? store->CreateSession() : std::nullopt;
    CHECK(private_session.has_value());
    if (private_session.has_value()) {
        WriteBytes(private_session->recording_path(), "private-video");
        const AclSummary private_acl = InspectAcl(private_session->directory);
        const AclSummary private_file_acl =
            InspectAcl(private_session->recording_path());
        CHECK(private_acl.protected_dacl);
        // Windows may choose TokenDefaultOwner (for example Administrators for
        // an elevated token), so descriptor ownership is not the access
        // invariant. The concrete TokenUser SID must have an explicit ACE.
        CHECK(private_acl.current_user_allowed);
        CHECK(private_acl.system_allowed);
        CHECK(!private_acl.everyone_allowed);
        CHECK(private_file_acl.current_user_allowed);
        CHECK(private_file_acl.system_allowed);
        CHECK(!private_file_acl.everyone_allowed);
        CHECK(private_file_acl.readable);
        CHECK(store->DeleteSession(*private_session));
    }

    const auto publication_session =
        store != nullptr ? store->CreateSession() : std::nullopt;
    CHECK(publication_session.has_value());
    if (!publication_session.has_value()) {
        return;
    }
    CHECK(easnap::app::ApplyClipboardPublicationSecurity(
        publication_session->directory));
    const std::wstring gif_path =
        publication_session->publication_path(RecordingFormat::Gif);
    WriteBytes(gif_path, "readable-gif");
    CHECK(easnap::app::ApplyClipboardPublicationSecurity(
        gif_path));
    const AclSummary directory_acl =
        InspectAcl(publication_session->directory);
    const AclSummary file_acl =
        InspectAcl(gif_path);
    CHECK(directory_acl.protected_dacl);
    CHECK(directory_acl.current_user_allowed);
    CHECK(directory_acl.system_allowed);
    CHECK(!directory_acl.everyone_allowed);
    CHECK(file_acl.protected_dacl);
    CHECK(file_acl.current_user_allowed);
    CHECK(file_acl.system_allowed);
    CHECK(!file_acl.everyone_allowed);
    CHECK(file_acl.readable);
    CHECK(store->DeleteSession(*publication_session));
}

void TestGifSaveAndClipboardPublication() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* probe = backend.get();
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));

    probe->persist_succeeds = false;
    CHECK(!controller.Save(L"C:\\session\\trimmed.gif",
                           L"C:\\Pictures\\saved.gif", RecordingFormat::Gif,
                           false));
    CHECK(store.delete_count == 0);
    CHECK(probe->last_format == RecordingFormat::Gif);

    probe->persist_succeeds = true;
    const auto destination =
        controller.PrepareClipboardDestination(RecordingFormat::Gif);
    CHECK(destination.has_value());
    CHECK(destination.has_value() &&
          std::filesystem::path(*destination).extension() == L".gif");
    CHECK(destination.has_value() && controller.PublishClipboardArtifact(
                                        nullptr, *destination,
                                        RecordingFormat::Gif));
    CHECK(probe->last_checked_format == RecordingFormat::Gif);
    CHECK(probe->clipboard_paths.has_value() &&
          probe->clipboard_paths->size() == 1 &&
          probe->clipboard_paths->front() == *destination);
    controller.OnClipboardChanged();
    CHECK(controller.clipboard_session().has_value());
    probe->clipboard_paths = std::vector<std::wstring>{};
    controller.OnClipboardChanged();
    CHECK(!controller.clipboard_session().has_value());
    CHECK(store.delete_count == 1);

    probe->clipboard_succeeds = false;
    const auto failed_destination =
        controller.PrepareClipboardDestination(RecordingFormat::Gif);
    CHECK(failed_destination.has_value());
    CHECK(failed_destination.has_value() &&
          !controller.PublishClipboardArtifact(nullptr, *failed_destination,
                                                RecordingFormat::Gif));
    CHECK(controller.prepared_clipboard_session().has_value());
    CHECK(store.delete_count == 1);
    controller.AbandonPreparedClipboardArtifact();
    CHECK(store.delete_count == 2);
}

void TestDurableSavePublication() {
    const std::filesystem::path directory = MakeTestDirectory();
    const auto source = directory / L"trimmed.mp4";
    const auto destination = directory / L"saved.mp4";
    WriteBytes(source, "valid-trim-result");

    CHECK(easnap::app::PersistRecordingFile(
        source.wstring(), destination.wstring(), RecordingFormat::Mp4, false));
    CHECK(ReadBytes(destination) == "valid-trim-result");
    CHECK(ReadBytes(source) == "valid-trim-result");

    WriteBytes(destination, "existing-user-file");
    CHECK(!easnap::app::PersistRecordingFile(
        source.wstring(), destination.wstring(), RecordingFormat::Mp4, false));
    CHECK(ReadBytes(destination) == "existing-user-file");
    CHECK(easnap::app::PersistRecordingFile(
        source.wstring(), destination.wstring(), RecordingFormat::Mp4, true));
    CHECK(ReadBytes(destination) == "valid-trim-result");

    const auto unavailable = directory / L"missing" / L"saved.mp4";
    CHECK(!easnap::app::PersistRecordingFile(
        source.wstring(), unavailable.wstring(), RecordingFormat::Mp4, false));
    CHECK(ReadBytes(source) == "valid-trim-result");
    CHECK(!easnap::app::IsValidRecordingDestination(
        (directory / L"wrong.mov").wstring(), RecordingFormat::Mp4));
    std::filesystem::remove_all(directory);
}

void TestSaveFailurePreservesSession() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* probe = backend.get();
    probe->persist_succeeds = false;
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));
    CHECK(!controller.Save(L"C:\\session\\trimmed.mp4",
                           L"C:\\Videos\\saved.mp4", RecordingFormat::Mp4,
                           false));
    CHECK(store.delete_count == 0);
    CHECK(probe->persist_count == 1);
    CHECK(!controller.Save(L"C:\\session\\trimmed.mp4",
                           L"C:\\fake\\easnap\\saved.mp4",
                           RecordingFormat::Mp4, false));
    CHECK(probe->persist_count == 1);
}

void TestCopyLifetimeAndFailure() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* probe = backend.get();
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));
    const auto staged = controller.StageClipboardArtifact(
        L"C:\\session\\trimmed.mp4", RecordingFormat::Mp4, false);
    CHECK(staged.has_value());
    CHECK(staged.has_value() &&
          controller.PublishClipboardArtifact(nullptr, *staged,
                                               RecordingFormat::Mp4));
    CHECK(controller.clipboard_session().has_value());
    CHECK(store.delete_count == 0);
    CHECK(staged.has_value() && probe->last_destination == *staged);
    CHECK(probe->move_count == 0);
    CHECK(probe->persist_count == 1);
    CHECK(probe->security_count == 2);

    controller.OnClipboardChanged();
    CHECK(controller.clipboard_session().has_value());
    CHECK(store.delete_count == 0);

    probe->clipboard_paths = std::vector<std::wstring>{};
    controller.OnClipboardChanged();
    CHECK(!controller.clipboard_session().has_value());
    CHECK(store.delete_count == 1);

    probe->clipboard_succeeds = false;
    const auto failed_staged = controller.StageClipboardArtifact(
        L"C:\\session\\trimmed.mp4", RecordingFormat::Mp4, false);
    CHECK(failed_staged.has_value());
    CHECK(failed_staged.has_value() &&
          !controller.PublishClipboardArtifact(nullptr, *failed_staged,
                                                RecordingFormat::Mp4));
    CHECK(controller.prepared_clipboard_session().has_value());
    controller.AbandonPreparedClipboardArtifact();
    CHECK(store.delete_count == 2);
    CHECK(store.created_count == 2);
}

void TestOptimizedClipboardStaging() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* probe = backend.get();
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));

    const auto moved = controller.StageClipboardArtifact(
        L"C:\\session\\derived-trim.mp4", RecordingFormat::Mp4, true);
    CHECK(moved.has_value());
    CHECK(probe->move_count == 1);
    CHECK(probe->persist_count == 0);
    controller.AbandonPreparedClipboardArtifact();

    probe->move_succeeds = false;
    const auto fallback = controller.StageClipboardArtifact(
        L"C:\\session\\derived-trim.mp4", RecordingFormat::Mp4, true);
    CHECK(fallback.has_value());
    CHECK(probe->move_count == 2);
    CHECK(probe->persist_count == 1);
    controller.AbandonPreparedClipboardArtifact();

    const int moves_before_direct = probe->move_count;
    const int copies_before_direct = probe->persist_count;
    const auto direct_destination =
        controller.PrepareClipboardDestination(RecordingFormat::Mp4);
    CHECK(direct_destination.has_value());
    CHECK(direct_destination.has_value() &&
          controller.PublishClipboardArtifact(nullptr, *direct_destination,
                                               RecordingFormat::Mp4));
    CHECK(probe->move_count == moves_before_direct);
    CHECK(probe->persist_count == copies_before_direct);

    probe->security_succeeds = false;
    const int deletes_before = store.delete_count;
    CHECK(!controller.StageClipboardArtifact(
               L"C:\\session\\recoverable.mp4", RecordingFormat::Mp4, true)
               .has_value());
    CHECK(probe->move_count == 2);
    CHECK(store.delete_count == deletes_before + 1);
}

void TestClipboardShutdownAndStaleProtection() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    FakeBackend* probe = backend.get();
    probe->clipboard_paths = std::vector<std::wstring>{
        L"C:\\fake\\easnap\\stale-session\\recording.mp4"};
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));
    CHECK(controller.CleanupStaleSessions() == 2);
    CHECK(store.cleanup_count == 1);
    CHECK(store.last_protected.size() == 1);
    CHECK(store.last_protected[0] == L"C:\\fake\\easnap\\stale-session");

    const auto staged = controller.StageClipboardArtifact(
        L"C:\\session\\trimmed.mp4", RecordingFormat::Mp4, false);
    CHECK(staged.has_value());
    CHECK(staged.has_value() &&
          controller.PublishClipboardArtifact(nullptr, *staged,
                                               RecordingFormat::Mp4));
    const std::wstring active = controller.clipboard_session()->directory;
    controller.Shutdown();
    CHECK(store.release_count == 1);
    CHECK(store.delete_count == 0);
    CHECK(store.released[0] == active);
}

void TestDiscardArtifactAndClosePolicy() {
    FakeSessionStore store;
    auto backend = std::make_unique<FakeBackend>();
    easnap::app::RecordingCompletionController controller(store,
                                                           std::move(backend));
    TemporarySession active{L"active", L"C:\\fake\\easnap\\active"};
    CHECK(controller.Discard(active));
    CHECK(store.delete_count == 1);

    using easnap::app::PlanTrimClose;
    using easnap::app::TrimCloseAction;
    CHECK(PlanTrimClose(false, false) == TrimCloseAction::KeepOpen);
    CHECK(PlanTrimClose(false, true) == TrimCloseAction::KeepOpen);
    CHECK(PlanTrimClose(true, false) == TrimCloseAction::DiscardNow);
    CHECK(PlanTrimClose(true, true) ==
          TrimCloseAction::CancelExportThenDiscard);

    easnap::app::TrimArtifact artifact{
        L"C:\\session\\trimmed.mp4", easnap::media::MediaTime{10},
        easnap::media::MediaTime{20}};
    CHECK(artifact.Matches(easnap::media::MediaTime{10},
                           easnap::media::MediaTime{20}));
    CHECK(!artifact.Matches(easnap::media::MediaTime{11},
                            easnap::media::MediaTime{20}));
    CHECK(!artifact.Matches(easnap::media::MediaTime{10},
                            easnap::media::MediaTime{19}));
    CHECK(artifact.Matches(easnap::media::MediaTime{10},
                           easnap::media::MediaTime{20}));

    using easnap::app::IsSessionTransitionLegal;
    using easnap::app::SessionState;
    CHECK(IsSessionTransitionLegal(SessionState::RecordingTrimming,
                                   SessionState::Idle));
    CHECK(IsSessionTransitionLegal(SessionState::Exporting,
                                   SessionState::Idle));
}

}  // namespace

int main() {
    TestLiveTemporarySessionLease();
    TestClipboardPublicationAcl();
    TestGifSaveAndClipboardPublication();
    TestDurableSavePublication();
    TestSaveFailurePreservesSession();
    TestCopyLifetimeAndFailure();
    TestOptimizedClipboardStaging();
    TestClipboardShutdownAndStaleProtection();
    TestDiscardArtifactAndClosePolicy();
    std::cout << "recording completion: " << g_passed << " passed, "
              << g_failed << " failed\n";
    return g_failed == 0 ? 0 : 1;
}
