#include "media/media_preview.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <new>
#include <sstream>
#include <utility>
#include <vector>

#include "media/preview_host_protocol.h"

namespace easnap::media {
namespace {

// Native playback can block inside MFPlay, including Shutdown. Owning it in a
// job-contained process makes retirement independent of COM/audio-driver locks.
class OwnedHandle final {
public:
    OwnedHandle() = default;
    explicit OwnedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~OwnedHandle() { reset(); }
    OwnedHandle(const OwnedHandle&) = delete;
    OwnedHandle& operator=(const OwnedHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }
private:
    HANDLE handle_{nullptr};
};

class ProcessAttributes final {
public:
    ~ProcessAttributes() {
        if (initialized_) ::DeleteProcThreadAttributeList(get());
    }
    bool Initialize(HANDLE input, HANDLE output) {
        SIZE_T bytes = 0;
        ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        storage_.resize(bytes);
        if (!::InitializeProcThreadAttributeList(get(), 1, 0, &bytes)) return false;
        initialized_ = true;
        handles_ = {input, output};
        return ::UpdateProcThreadAttribute(get(), 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
            sizeof(handles_), nullptr, nullptr) != FALSE;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST get() noexcept {
        return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
    }
private:
    std::vector<unsigned char> storage_;
    std::array<HANDLE, 2> handles_{};
    bool initialized_{false};
};

class ProcessMediaPreview final : public IMediaPreview {
public:
    ~ProcessMediaPreview() override { Close(); }

    bool Open(const std::wstring& source_path, HWND video_window,
              HWND notify_window, UINT notify_message) override {
        Close();
        notify_window_ = notify_window;
        notify_message_ = notify_message;
        if (process_.get() != nullptr) {
            // A prior termination request has not reached a signaled process.
            // Keep its ownership and identity; a second host is not permitted.
            Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), MediaPreviewOperation::Open, false);
            return false;
        }
        if (source_path.empty() ||
            source_path.size() > preview_host::kMaximumPathCharacters ||
            source_path.find(L'\0') != std::wstring::npos ||
            !::IsWindow(video_window) || !::IsWindow(notify_window) ||
            notify_message < WM_APP) {
            Fail(E_INVALIDARG, MediaPreviewOperation::Open);
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(source_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            Fail(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), MediaPreviewOperation::Open);
            return false;
        }
        try {
            source_path_ = source_path;
            video_window_ = video_window;
            current_.state = MediaPreviewState::Opening;
            ++generation_;
            Trace(L"open");
            if (!CreateOwnerWindow()) {
                Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
                return false;
            }
            return Launch();
        } catch (const std::bad_alloc&) {
            Fail(E_OUTOFMEMORY, MediaPreviewOperation::Open);
            return false;
        }
    }

    void Close() noexcept override {
        if (owner_window_ != nullptr) {
            ::KillTimer(owner_window_, kPollTimer);
            ::DestroyWindow(owner_window_);
            owner_window_ = nullptr;
        }
        resize_poll_posted_ = false;
        const bool retired = Retire(true);
        source_path_.clear();
        video_window_ = nullptr;
        notify_window_ = nullptr;
        notify_message_ = 0;
        current_ = {};
        if (!retired) {
            current_.state = MediaPreviewState::Failed;
            current_.error = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            current_.failed_operation = MediaPreviewOperation::Playback;
        }
        desired_play_ = false;
        exclusive_end_ = false;
        desired_position_ = MediaTime::zero();
        intent_sequence_ = position_sequence_ = acknowledged_position_ = 0;
        sent_intent_ = sent_position_ = acknowledged_intent_ = 0;
        recovery_attempts_ = 0;
        native_trace_.fill(L'\0');
        native_trace_length_ = 0;
        retired_trace_length_ = 0;
        Trace(retired ? L"close" : L"close-retirement-pending");
    }

    bool Play() override {
        if (!CanRequest()) return false;
        const auto state = snapshot();
        if (state.state == MediaPreviewState::Ended ||
            (state.duration > MediaTime::zero() && state.position >= state.duration)) {
            return PlayFrom(MediaTime::zero());
        }
        desired_play_ = true;
        return Changed(L"play");
    }

    bool Pause() override {
        if (!CanRequest()) return false;
        desired_play_ = false;
        return Changed(L"pause");
    }

    bool Seek(MediaTime position) override { return Position(position, desired_play_, L"seek"); }
    bool PlayFrom(MediaTime position) override { return Position(position, true, L"play-from"); }
    bool PauseAt(MediaTime position, bool exclusive_end = true) override {
        return Position(position, false, L"pause-at", exclusive_end);
    }

    void UpdateVideo() noexcept override {
        if (!CanRequest() || video_window_ == nullptr) return;
        RECT bounds{};
        if (::GetClientRect(video_window_, &bounds)) {
            const auto width = (std::max)(1L, bounds.right);
            const auto height = (std::max)(1L, bounds.bottom);
            if (width == resize_width_ && height == resize_height_) return;
            resize_width_ = width;
            resize_height_ = height;
            ++resize_sequence_;
            // Cross-process child windows share input queues, so even
            // SWP_ASYNCWINDOWPOS can block. The host alone positions its window.
            SendResize();
            // WM_TIMER is low priority and may be deferred by continuous
            // native input/painting. Consume available acknowledgements on one
            // coalesced owner-thread message so subsequent sizes can progress.
            // Never poll recursively in layout or continuously repost while
            // waiting for the helper; the existing timer remains the fallback.
            if (!resize_poll_posted_ && owner_window_ != nullptr) {
                resize_poll_posted_ = ::PostMessageW(owner_window_, kResizePollMessage, 0, 0) != FALSE;
            }
        }
    }

    MediaTime position() const noexcept override { return snapshot().position; }

    MediaPreviewSnapshot snapshot() const noexcept override {
        auto result = current_;
        result.player_generation = generation_;
        result.backend_process_id = process_id_;
        result.backend_thread_id = process_thread_id_;
        result.intent_sequence = intent_sequence_;
        if (result.state == MediaPreviewState::Closed ||
            result.state == MediaPreviewState::Failed) return result;
        result.play_requested = desired_play_;
        if (intent_sequence_ > acknowledged_intent_) result.command_pending = true;
        if (position_sequence_ > acknowledged_position_) {
            result.position = desired_position_;
            result.position_pending = true;
        }
        if (sent_intent_ > acknowledged_intent_) {
            result.command_age_ms = (std::max)(result.command_age_ms,
                ::GetTickCount64() - sent_at_ms_);
        }
        return result;
    }

    std::wstring diagnostics() const override {
        const auto state = snapshot();
        std::wostringstream output;
        output << L"preview-process pid=" << process_id_
               << L" generation=" << generation_
               << L" intent=" << intent_sequence_
               << L" acknowledged=" << acknowledged_intent_
               << L" heartbeat_age_ms=" << (::GetTickCount64() - heartbeat_ms_)
               << L" state=" << static_cast<int>(state.state)
               << L" position=" << state.position.count()
               << L" native=" << state.native_position.count()
               << L" native_valid=" << state.native_position_valid
               << L" native_playing=" << state.native_playing
               << L" command_pending=" << state.command_pending
               << L" position_pending=" << state.position_pending << L'\n';
        const size_t count = (std::min)(trace_count_, trace_.size());
        for (size_t index = 0; index < count; ++index) {
            const auto& entry = trace_[(trace_count_ - count + index) % trace_.size()];
            output << entry.time_ms << L" " << entry.event
                   << L" gen=" << entry.generation << L" intent=" << entry.intent
                   << L" state=" << static_cast<int>(entry.state)
                   << L" position=" << entry.position.count()
                   << L" native=" << entry.native_position.count()
                   << L" play_requested=" << entry.play_requested
                   << L" pending=" << entry.pending
                   << L" hr=" << std::hex << static_cast<unsigned long>(entry.error)
                   << std::dec << L'\n';
        }
        output << L"native host latest bounded trace:\n";
        output.write(native_trace_.data(), static_cast<std::streamsize>(native_trace_length_));
        if (retired_trace_length_ != 0) {
            output << L"\nretired host generation=" << retired_generation_
                   << L" pid=" << retired_process_id_ << L" bounded trace:\n";
            output.write(retired_trace_.data(),
                         static_cast<std::streamsize>(retired_trace_length_));
        }
        return output.str();
    }

private:
    static constexpr UINT_PTR kPollTimer = 1;
    static constexpr UINT kResizePollMessage = WM_APP + 1;
    static constexpr ULONGLONG kHeartbeatTimeoutMs = 3'000;
    static constexpr ULONGLONG kIntentTimeoutMs = 3'000;
    static constexpr ULONGLONG kOpenTimeoutMs = 5'000;
    struct TraceEntry final {
        ULONGLONG time_ms{0};
        const wchar_t* event{L""};
        uint64_t generation{0};
        uint64_t intent{0};
        MediaPreviewState state{};
        MediaTime position{0};
        MediaTime native_position{0};
        bool play_requested{false};
        bool pending{false};
        HRESULT error{S_OK};
    };

    static LRESULT CALLBACK OwnerProcedure(HWND window, UINT message,
                                          WPARAM wparam, LPARAM lparam) noexcept {
        auto* owner = reinterpret_cast<ProcessMediaPreview*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            owner = static_cast<ProcessMediaPreview*>(
                reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(owner));
        }
        if (message == WM_TIMER && wparam == kPollTimer && owner != nullptr) {
            owner->Poll();
            return 0;
        }
        if (message == kResizePollMessage && owner != nullptr) {
            owner->resize_poll_posted_ = false;
            owner->Poll();
            return 0;
        }
        return ::DefWindowProcW(window, message, wparam, lparam);
    }

    bool CreateOwnerWindow() noexcept {
        WNDCLASSW definition{};
        definition.lpfnWndProc = OwnerProcedure;
        definition.hInstance = ::GetModuleHandleW(nullptr);
        definition.lpszClassName = L"EasnapPreviewProcessOwner";
        if (!::RegisterClassW(&definition) && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }
        owner_window_ = ::CreateWindowExW(0, definition.lpszClassName, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, definition.hInstance, this);
        if (owner_window_ == nullptr) return false;
        return ::SetTimer(owner_window_, kPollTimer, 100, nullptr) != 0;
    }

    bool CanRequest() const noexcept {
        return process_.get() != nullptr && current_.state != MediaPreviewState::Failed &&
               current_.state != MediaPreviewState::Closed;
    }

    bool Position(MediaTime position, bool play, const wchar_t* event, bool exclusive_end = false) {
        if (!CanRequest()) return false;
        desired_position_ = (std::max)(MediaTime::zero(), position);
        if (current_.duration > MediaTime::zero()) {
            desired_position_ = (std::min)(desired_position_, current_.duration);
        }
        ++position_sequence_;
        desired_play_ = play;
        exclusive_end_ = exclusive_end;
        return Changed(event);
    }

    bool Changed(const wchar_t* event) noexcept {
        ++intent_sequence_;
        stable_since_ms_ = 0;
        Trace(event);
        const bool result = SendIntent();
        Notify();
        return result;
    }

    bool Launch() {
        std::array<wchar_t, 32'768> executable{};
        const DWORD length = ::GetModuleFileNameW(nullptr, executable.data(),
                                                static_cast<DWORD>(executable.size()));
        if (length == 0 || length >= executable.size()) {
            Fail(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME), MediaPreviewOperation::Open);
            return false;
        }
        std::wstring path(executable.data(), length);
        const auto separator = path.find_last_of(L"\\/");
        if (separator == std::wstring::npos) {
            Fail(HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME), MediaPreviewOperation::Open);
            return false;
        }
        path.resize(separator + 1);
        path.append(L"EasnapPreviewHost.exe");
        std::wstring command_line = L"\"" + path + L"\"";

        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        HANDLE child_input_raw = nullptr;
        HANDLE parent_output_raw = nullptr;
        if (!::CreatePipe(&child_input_raw, &parent_output_raw, &security,
                         2 * preview_host::kMaximumPacketSize)) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        OwnedHandle child_input(child_input_raw);
        to_child_.reset(parent_output_raw);
        HANDLE parent_input_raw = nullptr;
        HANDLE child_output_raw = nullptr;
        if (!::CreatePipe(&parent_input_raw, &child_output_raw, &security,
                         2 * preview_host::kMaximumPacketSize)) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        OwnedHandle child_output(child_output_raw);
        from_child_.reset(parent_input_raw);
        if (!::SetHandleInformation(to_child_.get(), HANDLE_FLAG_INHERIT, 0) ||
            !::SetHandleInformation(from_child_.get(), HANDLE_FLAG_INHERIT, 0)) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        ProcessAttributes attributes;
        if (!attributes.Initialize(child_input.get(), child_output.get())) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        job_.reset(::CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job_.get() == nullptr || !::SetInformationJobObject(job_.get(),
                JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdInput = child_input.get();
        startup.StartupInfo.hStdOutput = child_output.get();
        startup.StartupInfo.hStdError = child_output.get();
        startup.lpAttributeList = attributes.get();
        PROCESS_INFORMATION information{};
        if (!::CreateProcessW(path.c_str(), command_line.data(), nullptr, nullptr,
                TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr, &startup.StartupInfo, &information)) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        process_.reset(information.hProcess);
        OwnedHandle thread(information.hThread);
        process_id_ = information.dwProcessId;
        process_thread_id_ = information.dwThreadId;
        if (!::AssignProcessToJobObject(job_.get(), process_.get()) ||
            ::ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        // These are now exclusively the child's copies. In particular, an
        // unexpected child exit must produce EOF instead of a retained writer.
        child_input.reset();
        child_output.reset();
        heartbeat_ms_ = opened_at_ms_ = ::GetTickCount64();
        sent_intent_ = acknowledged_intent_ = sent_position_ = acknowledged_position_ = 0;
        stable_since_ms_ = 0;
        current_.state = MediaPreviewState::Opening;
        current_.error = S_OK;
        current_.failed_operation = MediaPreviewOperation::None;
        current_.native_position_valid = false;
        current_.native_playing = false;
        current_.command_pending = false;
        current_.position_pending = false;
        current_.recovery_requested = false;

        preview_host::OpenPacket packet{};
        packet.parent_window = reinterpret_cast<uintptr_t>(video_window_);
        packet.path_length = static_cast<uint32_t>(source_path_.size());
        packet.header.size = static_cast<uint32_t>(
            sizeof(packet) + source_path_.size() * sizeof(wchar_t));
        RECT bounds{};
        ::GetClientRect(video_window_, &bounds);
        packet.width = (std::max)(1L, bounds.right);
        packet.height = (std::max)(1L, bounds.bottom);
        resize_width_ = packet.width;
        resize_height_ = packet.height;
        resize_sequence_ = sent_resize_ = acknowledged_resize_ = 0;
        received_snapshot_ = 0;
        last_snapshot_sent_ms_ = 0;
        snapshot_refresh_started_ms_ = 0;
        std::vector<unsigned char> bytes(packet.header.size);
        std::memcpy(bytes.data(), &packet, sizeof(packet));
        std::memcpy(bytes.data() + sizeof(packet), source_path_.data(),
                    source_path_.size() * sizeof(wchar_t));
        if (!WritePacket(bytes.data(), static_cast<DWORD>(bytes.size()))) {
            Fail(HRESULT_FROM_WIN32(::GetLastError()), MediaPreviewOperation::Open);
            return false;
        }
        Trace(L"host-launched");
        Notify();
        return SendIntent();
    }

    bool WritePacket(const void* packet, DWORD size) noexcept {
        DWORD written = 0;
        return ::WriteFile(to_child_.get(), packet, size, &written, nullptr) && written == size;
    }

    bool SendIntent() noexcept {
        // Never fill a pipe from the editor thread: one small intent can be
        // outstanding, and rapid requests only replace the local desired state.
        if (sent_intent_ > acknowledged_intent_ || sent_intent_ == intent_sequence_) return true;
        preview_host::IntentPacket packet{};
        packet.intent_sequence = intent_sequence_;
        packet.position_sequence = position_sequence_;
        packet.position = desired_position_.count();
        packet.play_requested = desired_play_ ? 1U : 0U;
        packet.exclusive_end = exclusive_end_ ? 1U : 0U;
        if (!WritePacket(&packet, sizeof(packet))) {
            // Poll owns recovery; accepting intent preserves it across an
            // already-dead host without replacing it reentrantly in this call.
            pipe_broken_ = true;
            return true;
        }
        sent_intent_ = intent_sequence_;
        sent_position_ = position_sequence_;
        sent_at_ms_ = ::GetTickCount64();
        Trace(L"intent-sent");
        return true;
    }

    void SendResize() noexcept {
        // One resize may accompany the one outstanding transport intent. All
        // further layout changes coalesce until this resize is acknowledged.
        if (sent_resize_ > acknowledged_resize_ || sent_resize_ == resize_sequence_) return;
        preview_host::ResizePacket packet{};
        packet.sequence = resize_sequence_;
        packet.width = resize_width_;
        packet.height = resize_height_;
        if (!WritePacket(&packet, sizeof(packet))) {
            pipe_broken_ = true;
            return;
        }
        sent_resize_ = resize_sequence_;
        resize_sent_at_ms_ = ::GetTickCount64();
    }

    void Poll() noexcept {
        try {
            PollImpl();
        } catch (...) {
            Fail(E_OUTOFMEMORY, MediaPreviewOperation::Playback);
        }
    }

    void PollImpl() {
        if (!CanRequest()) return;
        // Every read is preceded by a complete-frame availability check. The
        // host can stall at any native call without blocking this message loop.
        // A host may publish its terminal error immediately before exiting;
        // consume that evidence before replacing an already-signaled process.
        for (unsigned index = 0; index < 64; ++index) {
            preview_host::PacketHeader header{};
            DWORD copied = 0;
            DWORD available = 0;
            if (!::PeekNamedPipe(from_child_.get(), &header, sizeof(header),
                                 &copied, &available, nullptr)) {
                Recover(L"pipe-closed");
                return;
            }
            if (copied < sizeof(header)) break;
            if (header.magic != preview_host::kProtocolMagic ||
                header.version != preview_host::kProtocolVersion ||
                header.type != preview_host::PacketType::Snapshot ||
                header.size != sizeof(preview_host::SnapshotPacket)) {
                Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), MediaPreviewOperation::Playback);
                return;
            }
            if (available < header.size) break;
            preview_host::SnapshotPacket packet{};
            DWORD read = 0;
            if (!::ReadFile(from_child_.get(), &packet, sizeof(packet), &read, nullptr) ||
                read != sizeof(packet)) {
                Recover(L"snapshot-read-failed");
                return;
            }
            if (packet.sequence != received_snapshot_ + 1 ||
                packet.acknowledged_intent > sent_intent_ ||
                packet.acknowledged_intent < acknowledged_intent_ ||
                packet.acknowledged_resize > sent_resize_ ||
                packet.acknowledged_resize < acknowledged_resize_ ||
                packet.diagnostics_length > native_trace_.size()) {
                Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), MediaPreviewOperation::Playback);
                return;
            }
            const auto observed_at = ::GetTickCount64();
            if (packet.sent_ms > observed_at || packet.sent_ms < last_snapshot_sent_ms_) {
                Fail(HRESULT_FROM_WIN32(ERROR_INVALID_DATA), MediaPreviewOperation::Playback);
                return;
            }
            last_snapshot_sent_ms_ = packet.sent_ms;
            heartbeat_ms_ = observed_at;
            if (observed_at - packet.sent_ms >= kIntentTimeoutMs && snapshot_refresh_started_ms_ == 0) {
                snapshot_refresh_started_ms_ = observed_at;
            } else if (snapshot_refresh_started_ms_ != 0 && packet.sent_ms >= snapshot_refresh_started_ms_) {
                snapshot_refresh_started_ms_ = 0;
            }
            received_snapshot_ = packet.sequence;
            if (packet.snapshot.state != MediaPreviewState::Failed) {
                preview_host::SnapshotAckPacket ack{};
                ack.sequence = received_snapshot_;
                if (!WritePacket(&ack, sizeof(ack))) pipe_broken_ = true;
            }
            const auto previous_state = current_.state;
            current_ = packet.snapshot;
            acknowledged_resize_ = packet.acknowledged_resize;
            native_trace_length_ = packet.diagnostics_length;
            std::copy_n(packet.diagnostics, native_trace_length_, native_trace_.data());
            if (packet.acknowledged_intent > acknowledged_intent_) {
                acknowledged_intent_ = packet.acknowledged_intent;
                acknowledged_position_ = sent_position_;
                Trace(L"intent-acknowledged");
            }
            const auto reported_window = reinterpret_cast<HWND>(
                static_cast<uintptr_t>(packet.video_window));
            if (reported_window != child_window_) {
                DWORD window_process = 0;
                ::GetWindowThreadProcessId(reported_window, &window_process);
                if (reported_window != nullptr && window_process == process_id_) {
                    child_window_ = reported_window;
                    UpdateVideo();
                }
            }
            if (current_.recovery_requested) {
                Recover(L"native-liveness-timeout");
                return;
            }
            if (current_.state == MediaPreviewState::Failed) {
                Fail(current_.error, current_.failed_operation);
                return;
            }
            if (acknowledged_intent_ == intent_sequence_ &&
                !current_.command_pending && current_.state == MediaPreviewState::Ended) {
                desired_play_ = false;
            }
            if (previous_state != current_.state) Trace(L"native-state");
            ObserveStablePlayback();
        }
        if (pipe_broken_ || ::WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0) {
            Recover(L"host-exited");
            return;
        }
        const auto now = ::GetTickCount64();
        const bool opening = current_.state == MediaPreviewState::Opening;
        // After a long modal-loop pause the queued snapshot predates the host's
        // latest acknowledgements. Returning credit only queues work in the
        // host; it need not publish a fresh snapshot before our next timer tick.
        // Keep a bounded response window across polls, ending on a snapshot
        // sent after credit was returned. Never extend it for another old packet.
        const bool waiting_for_refresh = snapshot_refresh_started_ms_ != 0 &&
            now - snapshot_refresh_started_ms_ < kHeartbeatTimeoutMs;
        if (!waiting_for_refresh && ((opening && now - opened_at_ms_ >= kOpenTimeoutMs) ||
            (!opening && now - heartbeat_ms_ >= kHeartbeatTimeoutMs))) {
            Recover(opening ? L"open-timeout" : L"heartbeat-timeout");
            return;
        }
        if (!waiting_for_refresh && !opening && sent_intent_ > acknowledged_intent_ &&
            now - sent_at_ms_ >= kIntentTimeoutMs) {
            Recover(L"intent-acknowledgement-timeout");
            return;
        }
        if (!waiting_for_refresh && sent_resize_ > acknowledged_resize_ &&
            now - resize_sent_at_ms_ >= kIntentTimeoutMs) {
            Recover(L"resize-acknowledgement-timeout");
            return;
        }
        SendIntent();
        SendResize();
        Notify();
    }

    void ObserveStablePlayback() noexcept {
        if (!desired_play_ || !current_.play_requested || current_.position_pending ||
            !current_.native_playing || !current_.native_position_valid ||
            current_.command_pending || acknowledged_intent_ != intent_sequence_) {
            stable_since_ms_ = 0;
            return;
        }
        const auto now = ::GetTickCount64();
        if (stable_since_ms_ == 0 || stable_intent_ != intent_sequence_ ||
            stable_command_ != current_.command_sequence ||
            stable_native_generation_ != current_.player_generation) {
            stable_since_ms_ = last_advance_ms_ = now;
            stable_intent_ = intent_sequence_;
            stable_command_ = current_.command_sequence;
            stable_native_generation_ = current_.player_generation;
            stable_position_ = stable_start_position_ = current_.native_position;
            return;
        }
        if (current_.native_position > stable_position_) {
            last_advance_ms_ = now;
            stable_position_ = current_.native_position;
        }
        if (now - last_advance_ms_ > 300) {
            stable_since_ms_ = 0;
        } else if (recovery_attempts_ != 0 && now - stable_since_ms_ >= 1'000 &&
                   current_.native_position - stable_start_position_ >=
                       std::chrono::milliseconds(500)) {
            recovery_attempts_ = 0;
            Trace(L"recovery-rearmed-by-progress");
        }
    }

    void Recover(const wchar_t* reason) {
        Trace(reason);
        if (recovery_attempts_ != 0) {
            Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                 (current_.state == MediaPreviewState::Opening ||
                  (current_.recovery_requested &&
                   current_.failed_operation == MediaPreviewOperation::Open))
                     ? MediaPreviewOperation::Open : MediaPreviewOperation::Playback);
            return;
        }
        // Native liveness failure preserves diagnostic state, but the parent
        // may already hold newer input than the acknowledged child intent.
        // Playback intent always remains parent-owned, even in Failed state.
        if (position_sequence_ <= acknowledged_position_) {
            desired_position_ = current_.position;
        }
        ++position_sequence_;
        ++intent_sequence_;
        ++recovery_attempts_;
        retired_trace_ = native_trace_;
        retired_trace_length_ = native_trace_length_;
        retired_generation_ = generation_;
        retired_process_id_ = process_id_;
        if (!Retire(false)) {
            Fail(HRESULT_FROM_WIN32(ERROR_TIMEOUT), MediaPreviewOperation::Playback, false);
            return;
        }
        ++generation_;
        pipe_broken_ = false;
        current_.position = desired_position_;
        Launch();
    }

    bool Retire(bool graceful) noexcept {
        child_window_ = nullptr;
        if (process_.get() != nullptr) {
            if (graceful && to_child_.get() != nullptr && !pipe_broken_) {
                const preview_host::ClosePacket close{};
                WritePacket(&close, sizeof(close));
            }
            to_child_.reset();
            from_child_.reset();
            const DWORD grace_ms = graceful ? 100 : 0;
            if (::WaitForSingleObject(process_.get(), grace_ms) != WAIT_OBJECT_0) {
                ::TerminateProcess(process_.get(), ERROR_PROCESS_ABORTED);
                // Closing the job is a second ownership-based termination path;
                // no cleanup operation waits on the child's native STA.
                job_.reset();
                if (::WaitForSingleObject(process_.get(), 900) != WAIT_OBJECT_0) {
                    Trace(L"host-retirement-deadline");
                    // Preserve the only authority capable of confirming this
                    // process is gone. Open and recovery refuse a replacement
                    // until a later bounded retirement observes its exit.
                    pipe_broken_ = true;
                    return false;
                }
            }
        }
        job_.reset();
        process_.reset();
        to_child_.reset();
        from_child_.reset();
        process_id_ = process_thread_id_ = 0;
        pipe_broken_ = false;
        return true;
    }

    void Fail(HRESULT error, MediaPreviewOperation operation,
              bool retire = true) noexcept {
        current_.state = MediaPreviewState::Failed;
        current_.error = FAILED(error) ? error : E_FAIL;
        current_.failed_operation = operation;
        current_.command_pending = false;
        current_.position_pending = false;
        current_.recovery_requested = false;
        current_.native_playing = false;
        current_.native_position_valid = false;
        current_.play_requested = desired_play_ = false;
        Trace(L"failed");
        if (retire) Retire(false);
        Notify();
    }

    void Notify() const noexcept {
        if (notify_window_ != nullptr && notify_message_ >= WM_APP) {
            ::PostMessageW(notify_window_, notify_message_, 0, 0);
        }
    }

    void Trace(const wchar_t* event) noexcept {
        trace_[trace_count_++ % trace_.size()] = {
            ::GetTickCount64(), event, generation_, intent_sequence_, current_.state,
            position_sequence_ > acknowledged_position_ ? desired_position_ : current_.position,
            current_.native_position, desired_play_,
            intent_sequence_ > acknowledged_intent_ || current_.command_pending, current_.error};
    }

    HWND owner_window_{nullptr};
    HWND video_window_{nullptr};
    HWND child_window_{nullptr};
    HWND notify_window_{nullptr};
    UINT notify_message_{0};
    OwnedHandle process_;
    OwnedHandle job_;
    OwnedHandle to_child_;
    OwnedHandle from_child_;
    DWORD process_id_{0};
    DWORD process_thread_id_{0};
    std::wstring source_path_;
    MediaPreviewSnapshot current_{};
    bool desired_play_{false};
    bool exclusive_end_{false};
    bool pipe_broken_{false};
    bool resize_poll_posted_{false};
    MediaTime desired_position_{0};
    uint64_t generation_{0};
    uint64_t intent_sequence_{0};
    uint64_t position_sequence_{0};
    uint64_t sent_intent_{0};
    uint64_t sent_position_{0};
    uint64_t acknowledged_intent_{0};
    uint64_t acknowledged_position_{0};
    int32_t resize_width_{1};
    int32_t resize_height_{1};
    uint64_t resize_sequence_{0};
    uint64_t sent_resize_{0};
    uint64_t acknowledged_resize_{0};
    uint64_t received_snapshot_{0};
    ULONGLONG last_snapshot_sent_ms_{0};
    ULONGLONG snapshot_refresh_started_ms_{0};
    ULONGLONG resize_sent_at_ms_{0};
    ULONGLONG sent_at_ms_{0};
    ULONGLONG heartbeat_ms_{0};
    ULONGLONG opened_at_ms_{0};
    unsigned recovery_attempts_{0};
    ULONGLONG stable_since_ms_{0};
    ULONGLONG last_advance_ms_{0};
    uint64_t stable_intent_{0};
    uint64_t stable_command_{0};
    uint64_t stable_native_generation_{0};
    MediaTime stable_position_{0};
    MediaTime stable_start_position_{0};
    std::array<TraceEntry, 64> trace_{};
    size_t trace_count_{0};
    std::array<wchar_t, 2'048> native_trace_{};
    size_t native_trace_length_{0};
    std::array<wchar_t, 2'048> retired_trace_{};
    size_t retired_trace_length_{0};
    uint64_t retired_generation_{0};
    DWORD retired_process_id_{0};
};

}  // namespace

std::unique_ptr<IMediaPreview> CreateMfPlayPreview() {
    return std::make_unique<ProcessMediaPreview>();
}

}  // namespace easnap::media
