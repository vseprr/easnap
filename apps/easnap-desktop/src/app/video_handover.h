// Easnap - a video handed to the running Easnap by a second Easnap.exe (ADR-040).
//
// Explorer's "Open with" starts Easnap.exe with the file. When Easnap already
// runs, the new process hands the path to the running one with WM_COPYDATA and
// ends. Any program on the desktop can send that message, so the receiver
// treats it as a request from the user, not as a command: it checks the
// payload, keeps one path, posts its own window a message and lets the
// coordinator act on it there, only while nothing else is going on.
#pragma once

#include <windows.h>

#include <chrono>
#include <optional>
#include <string>

namespace easnap::app {

/// COPYDATASTRUCT::dwData of a hand-over ("EVOP").
inline constexpr ULONG_PTR kOpenVideoHandOverMagic = 0x45564F50;
/// The largest payload taken, terminator included: 16,383 characters.
inline constexpr DWORD kMaxHandOverBytes = 32 * 1024;
/// Posted by the receiving window to itself once it has kept a request.
inline constexpr UINT kMsgOpenVideoRequest = WM_APP + 19;

/// Pure: the path a well-formed hand-over carries. Well-formed is the magic, a
/// size that is a whole number of UTF-16 units and at most kMaxHandOverBytes,
/// one terminating NUL with none before it, and an absolute local spelling
/// (a drive letter, or "\\?\" and a drive letter; never a UNC path or a URL).
/// Nothing otherwise. The file system is not consulted.
[[nodiscard]] std::optional<std::wstring> ParseHandOver(const COPYDATASTRUCT& data);

/// The receiving side, on the thread of the window that receives the message.
/// At most one request waits at a time.
class OpenVideoRequests final {
public:
    /// Handles WM_COPYDATA. A well-formed request (ParseHandOver) is kept and
    /// kMsgOpenVideoRequest posted to `window`, when `accepting` and no other
    /// request waits. Everything else is dropped with a log line and nothing
    /// is shown. The disk is not consulted on this thread: the sender has
    /// checked the location and the probe checks it again on its worker. True
    /// when the request was kept; the window procedure returns that to the
    /// sender.
    bool OnCopyData(HWND window, const COPYDATASTRUCT* data, bool accepting);
    /// Keeps a request this process received itself (its own command line)
    /// and posts kMsgOpenVideoRequest to `window`. False while another waits.
    bool Queue(HWND window, std::wstring path);
    /// Handles kMsgOpenVideoRequest: the waiting request, taken once.
    [[nodiscard]] std::optional<std::wstring> Take();
    [[nodiscard]] bool waiting() const noexcept { return pending_.has_value(); }

private:
    std::optional<std::wstring> pending_;
};

enum class HandOverResult {
    /// The running Easnap kept the request.
    Taken,
    /// It answered no, did not answer in time, or the path is too long.
    Refused,
    /// No window of the class belongs to a process of this executable's name.
    NoRecipient,
};

/// The sending side: finds the top-level window of `window_class`, checks that
/// its process runs an executable of this process's file name, lets it take
/// the foreground and sends it `path` with WM_COPYDATA. Waits at most
/// `timeout` for the answer.
[[nodiscard]] HandOverResult HandOverOpenVideo(const wchar_t* window_class, const std::wstring& path,
                                               std::chrono::milliseconds timeout);

}  // namespace easnap::app
