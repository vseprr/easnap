// Easnap - a video handed to the running Easnap by a second Easnap.exe (ADR-040).
#include "app/video_handover.h"

#include <cwchar>
#include <string_view>
#include <utility>

#include "media/video_probe.h"
#include "platform/platform_utils.h"

namespace easnap::app {
namespace {

/// The last component of a path; the whole text when it has no separator.
std::wstring_view FileNameOf(std::wstring_view path) noexcept {
    const std::size_t separator = path.find_last_of(L"\\/");
    return separator == std::wstring_view::npos ? path : path.substr(separator + 1);
}

std::wstring ProcessImagePath(HANDLE process) {
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (::QueryFullProcessImageNameW(process, 0, path.data(), &size) == FALSE) {
        return {};
    }
    path.resize(size);
    return path;
}

std::wstring OwnImagePath() {
    std::wstring path(32768, L'\0');
    const DWORD size = ::GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (size == 0 || size >= path.size()) {
        return {};
    }
    path.resize(size);
    return path;
}

/// The window's process runs an executable of the same file name as this one:
/// Easnap.exe, or in a test, the test's own executable.
bool RunsThisExecutable(DWORD process_id) {
    const platform::UniqueHandle process(::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process) {
        return false;
    }
    const std::wstring theirs = ProcessImagePath(process.get());
    const std::wstring ours = OwnImagePath();
    if (theirs.empty() || ours.empty()) {
        return false;
    }
    const std::wstring_view their_name = FileNameOf(theirs);
    const std::wstring_view our_name = FileNameOf(ours);
    return ::CompareStringOrdinal(their_name.data(), static_cast<int>(their_name.size()), our_name.data(),
                                  static_cast<int>(our_name.size()), TRUE) == CSTR_EQUAL;
}

}  // namespace

std::optional<std::wstring> ParseHandOver(const COPYDATASTRUCT& data) {
    if (data.dwData != kOpenVideoHandOverMagic || data.lpData == nullptr) {
        return std::nullopt;
    }
    // At least one character and the terminator, in whole UTF-16 units.
    if (data.cbData < 2 * sizeof(wchar_t) || data.cbData > kMaxHandOverBytes || data.cbData % sizeof(wchar_t) != 0) {
        return std::nullopt;
    }
    const auto* const text = static_cast<const wchar_t*>(data.lpData);
    const std::size_t length = data.cbData / sizeof(wchar_t) - 1;
    if (text[length] != L'\0' || std::wcslen(text) != length) {
        return std::nullopt;
    }
    std::wstring path(text, length);
    if (media::CheckVideoPathSyntax(path) != media::VideoPathProblem::None) {
        return std::nullopt;
    }
    return path;
}

bool OpenVideoRequests::OnCopyData(HWND window, const COPYDATASTRUCT* data, bool accepting) {
    if (data == nullptr) {
        platform::LogMessage(L"[Open] A hand-over without data was dropped");
        return false;
    }
    std::optional<std::wstring> path = ParseHandOver(*data);
    if (!path.has_value()) {
        platform::LogMessage(L"[Open] A malformed hand-over was dropped (%lu bytes)", data->cbData);
        return false;
    }
    // The disk is not consulted here, on the window's thread: the sender has
    // checked the location, and the probe checks it again on its worker.
    if (!accepting || pending_.has_value()) {
        platform::LogMessage(L"[Open] A hand-over was dropped: Easnap is busy");
        return false;
    }
    return Queue(window, std::move(*path));
}

bool OpenVideoRequests::Queue(HWND window, std::wstring path) {
    if (pending_.has_value() || path.empty()) {
        return false;
    }
    pending_ = std::move(path);
    if (::PostMessageW(window, kMsgOpenVideoRequest, 0, 0) == FALSE) {
        platform::LogMessage(L"[Open] A video request could not be queued (error %lu)", ::GetLastError());
        pending_.reset();
        return false;
    }
    return true;
}

std::optional<std::wstring> OpenVideoRequests::Take() {
    std::optional<std::wstring> request = std::move(pending_);
    pending_.reset();
    return request;
}

HandOverResult HandOverOpenVideo(const wchar_t* window_class, const std::wstring& path,
                                 std::chrono::milliseconds timeout) {
    HWND recipient = ::FindWindowW(window_class, nullptr);
    DWORD process_id = 0;
    if (recipient == nullptr || ::GetWindowThreadProcessId(recipient, &process_id) == 0 ||
        !RunsThisExecutable(process_id)) {
        return HandOverResult::NoRecipient;
    }
    const std::size_t bytes = (path.size() + 1) * sizeof(wchar_t);
    if (path.empty() || bytes > kMaxHandOverBytes) {
        return HandOverResult::Refused;
    }
    // This process was started by the user (Explorer gave it the foreground);
    // the trim view the running Easnap opens may take it over.
    ::AllowSetForegroundWindow(process_id);
    COPYDATASTRUCT data{};
    data.dwData = kOpenVideoHandOverMagic;
    data.cbData = static_cast<DWORD>(bytes);
    data.lpData = const_cast<wchar_t*>(path.c_str());
    DWORD_PTR answer = FALSE;
    const UINT wait = static_cast<UINT>(timeout.count() > 0 ? timeout.count() : 0);
    if (::SendMessageTimeoutW(recipient, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                              SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, wait, &answer) == 0) {
        return HandOverResult::Refused;
    }
    return answer != FALSE ? HandOverResult::Taken : HandOverResult::Refused;
}

}  // namespace easnap::app
