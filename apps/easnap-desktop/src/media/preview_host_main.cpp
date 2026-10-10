#include "media/preview_host_protocol.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <string>

namespace easnap::media {
namespace {

namespace protocol = preview_host;
constexpr UINT kPreviewChangedMessage = WM_APP + 1;
constexpr wchar_t kHostWindowClass[] = L"Easnap.NativePreviewHost";
constexpr ULONGLONG kSnapshotIntervalMs = 50;

// MFPlay and every native object stay on this process's COM owner thread. A
// driver blocked inside a call or Shutdown cannot hold the desktop's UI thread
// or any desktop-owned COM object. The parent owns the process lifetime.
class PreviewHost final {
public:
    PreviewHost(HANDLE input, HANDLE output) noexcept
        : input_(input), output_(output) {}

    ~PreviewHost() {
        if (preview_) preview_->Close();
        preview_.reset();
        if (video_window_ != nullptr) ::DestroyWindow(video_window_);
        if (owner_window_ != nullptr) ::DestroyWindow(owner_window_);
    }

    int Run() {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = ::GetModuleHandleW(nullptr);
        window_class.lpszClassName = kHostWindowClass;
        window_class.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        if (::RegisterClassW(&window_class) == 0) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            static_cast<void>(SendSnapshot());
            return 1;
        }
        owner_window_ = ::CreateWindowExW(
            0, kHostWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
            nullptr, window_class.hInstance, this);
        if (owner_window_ == nullptr) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            static_cast<void>(SendSnapshot());
            return 1;
        }

        ULONGLONG next_snapshot = 0;
        while (!closing_) {
            // Bound each drain so callbacks and snapshots cannot be starved by
            // input. Applying after the drain coalesces to the newest intent.
            for (unsigned packet_count = 0; packet_count < 64; ++packet_count) {
                const ReadResult result = ReadPacket();
                if (result == ReadResult::Unavailable) break;
                if (result == ReadResult::Disconnected) return 0;
                if (result == ReadResult::Invalid) {
                    Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                         MediaPreviewOperation::Playback);
                    static_cast<void>(SendSnapshot());
                    return 1;
                }
                if (closing_) break;
            }
            if (closing_) break;
            PumpMessages();
            if (closing_) break;
            ApplyIntent();
            if (failed_) {
                static_cast<void>(SendSnapshot());
                return 1;
            }

            const ULONGLONG now = ::GetTickCount64();
            if (opened_ && now >= next_snapshot) {
                if (!SendSnapshot()) return 0;
                next_snapshot = now + kSnapshotIntervalMs;
            }
            // Anonymous pipes have no overlapped readiness event; poll only
            // this tiny private protocol and remain responsive to COM messages.
            static_cast<void>(::MsgWaitForMultipleObjectsEx(
                0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE));
        }
        return 0;
    }

    void ReportException(HRESULT error) noexcept {
        Fail(error, MediaPreviewOperation::Playback);
        try {
            static_cast<void>(SendSnapshot());
        } catch (...) {
            // Process exit is itself a failure signal to the supervising parent.
        }
    }

private:
    enum class ReadResult { Unavailable, Received, Disconnected, Invalid };

    static LRESULT CALLBACK WindowProc(HWND window, UINT message,
                                       WPARAM wparam, LPARAM lparam) {
        auto* host = reinterpret_cast<PreviewHost*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            host = static_cast<PreviewHost*>(create->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                               reinterpret_cast<LONG_PTR>(host));
        }
        if (host == nullptr) return ::DefWindowProcW(window, message, wparam, lparam);
        if (message == kPreviewChangedMessage) return 0;
        // Preserve the presenter's last frame until its resize repaint.
        if (message == WM_ERASEBKGND && window == host->video_window_) return 1;
        if (message == WM_SIZE && window == host->video_window_) {
            if (host->preview_) host->preview_->UpdateVideo();
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            if (window == host->video_window_ && host->preview_) {
                host->preview_->UpdateVideo();
            }
            ::EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_CLOSE) {
            host->closing_ = true;
            return 0;
        }
        if (message == WM_NCDESTROY) {
            if (window == host->video_window_) {
                host->video_window_ = nullptr;
                host->closing_ = true;
            }
            if (window == host->owner_window_) host->owner_window_ = nullptr;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    void PumpMessages() {
        MSG message{};
        for (unsigned count = 0; count < 128 &&
             ::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) {
                closing_ = true;
                return;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
    }

    ReadResult ReadPacket() {
        protocol::PacketHeader header{};
        DWORD available = 0;
        DWORD peeked = 0;
        if (!::PeekNamedPipe(input_, &header, sizeof(header), &peeked,
                             &available, nullptr)) {
            return ReadResult::Disconnected;
        }
        if (peeked < sizeof(header)) return ReadResult::Unavailable;
        if (header.magic != protocol::kProtocolMagic ||
            header.version != protocol::kProtocolVersion ||
            header.size < sizeof(header) ||
            header.size > protocol::kMaximumPacketSize) {
            return ReadResult::Invalid;
        }
        if (available < header.size) return ReadResult::Unavailable;
        DWORD received = 0;
        if (!::ReadFile(input_, packet_buffer_.data(), header.size,
                        &received, nullptr) || received != header.size) {
            return ReadResult::Disconnected;
        }
        switch (header.type) {
            case protocol::PacketType::Open:
                return OpenSource(header.size) ? ReadResult::Received : ReadResult::Invalid;
            case protocol::PacketType::Intent: {
                if (!opened_ || header.size != sizeof(protocol::IntentPacket)) {
                    return ReadResult::Invalid;
                }
                protocol::IntentPacket intent{};
                std::memcpy(&intent, packet_buffer_.data(), sizeof(intent));
                if (intent.intent_sequence == 0 || intent.play_requested > 1 || intent.exclusive_end > 1 ||
                    intent.position < 0 ||
                    intent.position_sequence < desired_.position_sequence) {
                    return ReadResult::Invalid;
                }
                if (intent.intent_sequence > desired_.intent_sequence) desired_ = intent;
                return ReadResult::Received;
            }
            case protocol::PacketType::Resize: {
                if (!opened_ || video_window_ == nullptr ||
                    header.size != sizeof(protocol::ResizePacket)) return ReadResult::Invalid;
                protocol::ResizePacket resize{};
                std::memcpy(&resize, packet_buffer_.data(), sizeof(resize));
                if (resize.sequence <= acknowledged_resize_ ||
                    resize.width <= 0 || resize.height <= 0) return ReadResult::Invalid;
                // A cross-process child can share an input queue with its parent.
                // Apply resize on this owning thread so any synchronous window
                // or decoder work remains inside the supervised helper process.
                if (!::SetWindowPos(video_window_, nullptr, 0, 0, resize.width,
                                    resize.height, SWP_NOACTIVATE | SWP_NOZORDER)) {
                    Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Playback);
                    return ReadResult::Received;
                }
                acknowledged_resize_ = resize.sequence;
                return ReadResult::Received;
            }
            case protocol::PacketType::Close:
                if (header.size != sizeof(protocol::ClosePacket)) return ReadResult::Invalid;
                closing_ = true;
                return ReadResult::Received;
            case protocol::PacketType::SnapshotAck: {
                if (header.size != sizeof(protocol::SnapshotAckPacket)) return ReadResult::Invalid;
                protocol::SnapshotAckPacket ack{};
                std::memcpy(&ack, packet_buffer_.data(), sizeof(ack));
                if (ack.sequence != sent_snapshot_ || ack.sequence <= acknowledged_snapshot_) {
                    return ReadResult::Invalid;
                }
                acknowledged_snapshot_ = ack.sequence;
                return ReadResult::Received;
            }
            default:
                return ReadResult::Invalid;
        }
    }

    bool OpenSource(uint32_t packet_size) {
        if (opened_ || packet_size < sizeof(protocol::OpenPacket)) return false;
        protocol::OpenPacket open{};
        std::memcpy(&open, packet_buffer_.data(), sizeof(open));
        if (open.path_length == 0 ||
            open.path_length > protocol::kMaximumPathCharacters ||
            packet_size != sizeof(open) + open.path_length * sizeof(wchar_t) ||
            open.width <= 0 || open.height <= 0) {
            return false;
        }
        std::wstring source(open.path_length, L'\0');
        std::memcpy(source.data(), packet_buffer_.data() + sizeof(open),
                    open.path_length * sizeof(wchar_t));
        if (source.find(L'\0') != std::wstring::npos) return false;
        const HWND parent = reinterpret_cast<HWND>(
            static_cast<uintptr_t>(open.parent_window));
        if (!::IsWindow(parent)) return false;
        opened_ = true;
        video_window_ = ::CreateWindowExW(
            WS_EX_NOACTIVATE, kHostWindowClass, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
            0, 0, open.width, open.height, parent, nullptr,
            ::GetModuleHandleW(nullptr), this);
        if (video_window_ == nullptr) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return true;
        }
        preview_ = CreateInProcessMfPlayPreview();
        if (!preview_) {
            Fail(E_OUTOFMEMORY, MediaPreviewOperation::Open);
            return true;
        }
        // Open failures remain the adapter's own operation/HRESULT snapshot.
        static_cast<void>(preview_->Open(source, video_window_, owner_window_,
                                         kPreviewChangedMessage));
        return true;
    }

    void ApplyIntent() {
        if (failed_ || !preview_ || desired_.intent_sequence <= acknowledged_intent_) return;
        const MediaPreviewSnapshot current = preview_->snapshot();
        if (current.state == MediaPreviewState::Opening ||
            current.state == MediaPreviewState::Closed ||
            current.state == MediaPreviewState::Failed) return;

        bool accepted = false;
        MediaPreviewOperation operation{};
        if (desired_.position_sequence > applied_position_sequence_) {
            operation = MediaPreviewOperation::Seek;
            const MediaTime position{desired_.position};
            accepted = desired_.play_requested != 0
                ? preview_->PlayFrom(position) : preview_->PauseAt(position, desired_.exclusive_end != 0);
        } else {
            operation = desired_.play_requested != 0
                ? MediaPreviewOperation::Play : MediaPreviewOperation::Pause;
            accepted = desired_.play_requested != 0 ? preview_->Play() : preview_->Pause();
        }
        if (accepted) {
            acknowledged_intent_ = desired_.intent_sequence;
            applied_position_sequence_ = desired_.position_sequence;
        } else if (preview_->snapshot().state != MediaPreviewState::Failed) {
            Fail(E_UNEXPECTED, operation);
        }
    }

    void Fail(HRESULT error, MediaPreviewOperation operation) noexcept {
        failed_ = true;
        failure_.state = MediaPreviewState::Failed;
        failure_.error = FAILED(error) ? error : E_FAIL;
        failure_.failed_operation = operation;
    }

    bool SendSnapshot() {
        // A synchronous anonymous-pipe write must never fill the pipe on the
        // thread owning the video HWND. Its input queue is attached to the
        // parent. Coalesce status until the parent consumes the previous one;
        // reserve one additional packet for a terminal error before exit.
        if (failed_ ? terminal_snapshot_sent_ : sent_snapshot_ > acknowledged_snapshot_) return true;
        protocol::SnapshotPacket packet{};
        packet.sequence = sent_snapshot_ + 1;
        packet.acknowledged_intent = acknowledged_intent_;
        packet.acknowledged_resize = acknowledged_resize_;
        packet.video_window = reinterpret_cast<uintptr_t>(video_window_);
        packet.snapshot = failed_ ? failure_
            : (preview_ ? preview_->snapshot() : MediaPreviewSnapshot{});
        if (video_window_ && !video_shown_ && packet.snapshot.state != MediaPreviewState::Opening &&
            packet.snapshot.state != MediaPreviewState::Closed && packet.snapshot.state != MediaPreviewState::Failed) {
            video_shown_ = true;
            ::ShowWindow(video_window_, SW_SHOWNOACTIVATE);
            ::InvalidateRect(video_window_, nullptr, FALSE);
        }
        if (preview_ && !failed_) {
            const std::wstring diagnostics = preview_->diagnostics();
            const size_t count = (std::min)(diagnostics.size(), std::size(packet.diagnostics));
            const size_t offset = diagnostics.size() - count;
            std::copy_n(diagnostics.data() + offset, count, packet.diagnostics);
            packet.diagnostics_length = static_cast<uint32_t>(count);
        }
        packet.sent_ms = ::GetTickCount64();
        DWORD sent = 0;
        if (!::WriteFile(output_, &packet, sizeof(packet), &sent, nullptr) || sent != sizeof(packet)) return false;
        sent_snapshot_ = packet.sequence;
        terminal_snapshot_sent_ = failed_;
        return true;
    }

    HANDLE input_{nullptr};
    HANDLE output_{nullptr};
    HWND owner_window_{nullptr};
    HWND video_window_{nullptr};
    std::unique_ptr<IMediaPreview> preview_;
    std::array<std::byte, protocol::kMaximumPacketSize> packet_buffer_{};
    protocol::IntentPacket desired_{};
    uint64_t acknowledged_intent_{0};
    uint64_t acknowledged_resize_{0};
    uint64_t sent_snapshot_{0};
    uint64_t acknowledged_snapshot_{0};
    uint64_t applied_position_sequence_{0};
    MediaPreviewSnapshot failure_{};
    bool opened_{false};
    bool failed_{false};
    bool closing_{false};
    bool terminal_snapshot_sent_{false};
    bool video_shown_{false};
};

}  // namespace
}  // namespace easnap::media

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    easnap::platform::RestrictDllSearchPath();
    const HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (input == nullptr || input == INVALID_HANDLE_VALUE ||
        output == nullptr || output == INVALID_HANDLE_VALUE ||
        ::GetFileType(input) != FILE_TYPE_PIPE || ::GetFileType(output) != FILE_TYPE_PIPE) {
        return 1;
    }
    easnap::platform::EnableDpiAwareness();
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) {
        easnap::media::preview_host::SnapshotPacket packet{};
        packet.sequence = 1;
        packet.sent_ms = ::GetTickCount64();
        packet.snapshot.state = easnap::media::MediaPreviewState::Failed;
        packet.snapshot.error = initialized;
        packet.snapshot.failed_operation = easnap::media::MediaPreviewOperation::Open;
        DWORD sent = 0;
        static_cast<void>(::WriteFile(output, &packet, sizeof(packet), &sent, nullptr));
        return 1;
    }
    int exit_code = 1;
    {
        easnap::media::PreviewHost host(input, output);
        try {
            exit_code = host.Run();
        } catch (const std::bad_alloc&) {
            host.ReportException(E_OUTOFMEMORY);
        } catch (...) {
            host.ReportException(E_UNEXPECTED);
        }
    }
    ::CoUninitialize();
    return exit_code;
}
