// The preview helper must remain responsive when a modal parent message loop
// services sent messages but temporarily does not dispatch its polling timer.
#include <atomic>
#include "native_test_monitor.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

#include <windows.h>
#include <mfapi.h>

#include "media/media_preview.h"
#include "preview_test_fixture.h"

namespace {

using easnap::media::MediaPreviewState;
using easnap::media::MediaTime;

int failures = 0;

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
    ++failures; } } while (false)

template <typename Predicate>
bool WaitFor(Predicate predicate, ULONGLONG timeout_ms = 5'000) {
    const auto deadline = ::GetTickCount64() + timeout_ms;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (predicate()) return true;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (::GetTickCount64() < deadline);
    return predicate();
}

struct ProbeResult final {
    std::atomic<bool> stop{false};
    std::atomic<bool> failed{false};
    std::atomic<unsigned int> replies{0};
    std::atomic<ULONGLONG> longest_reply_ms{0};
    std::atomic<ULONGLONG> failure_at_ms{0};
    std::atomic<DWORD> error{ERROR_SUCCESS};
};

void ProbeHelper(HWND helper, ULONGLONG started, ProbeResult& result) {
    // A helper child and its parent can share an input queue, for which Win32
    // ignores SendMessageTimeout's deadline. This windowless probe thread is
    // deliberately not attached to either queue, keeping failure bounded.
    while (!result.stop.load()) {
        DWORD_PTR ignored = 0;
        const auto before = ::GetTickCount64();
        ::SetLastError(ERROR_SUCCESS);
        const auto replied = ::SendMessageTimeoutW(helper, WM_NULL, 0, 0,
            SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 250, &ignored);
        const auto error = ::GetLastError();
        const auto elapsed = ::GetTickCount64() - before;
        const auto longest = result.longest_reply_ms.load();
        if (elapsed > longest) result.longest_reply_ms.store(elapsed);
        if (!replied) {
            result.error.store(error);
            result.failure_at_ms.store(::GetTickCount64() - started);
            result.failed.store(true);
            return;
        }
        ++result.replies;
        std::this_thread::sleep_for(std::chrono::milliseconds(75));
    }
}

unsigned int timer_dispatches = 0;

void CALLBACK TimerDispatch(HWND, UINT, UINT_PTR, DWORD) {
    ++timer_dispatches;
}

void ResizeWhileTimersAreDeferred(easnap::media::IMediaPreview& preview, HWND parent, HWND helper) {
    // Native input/paint has priority over WM_TIMER. Keep servicing posted and
    // sent messages while deliberately deferring timers, as sustained input
    // can do. Resize must make repeated progress, not stop after its first ack.
    const auto before=preview.snapshot();
    timer_dispatches=0;
    const UINT_PTR sentinel=::SetTimer(nullptr,0,25,TimerDispatch);
    CHECK(sentinel!=0);
    const auto started=::GetTickCount64();
    ULONGLONG next_resize=started;
    unsigned requests=0,observed_resizes=0,posted=0;
    RECT previous{};CHECK(::GetClientRect(helper,&previous));
    while(::GetTickCount64()-started<1000) {
        if(::GetTickCount64()>=next_resize) {
            ++requests;
            CHECK(::SetWindowPos(parent,nullptr,0,0,640+static_cast<int>(requests)*4,
                360+static_cast<int>(requests)*2,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOREDRAW));
            preview.UpdateVideo();
            next_resize=::GetTickCount64()+16;
        }
        MSG message{};
        while(::PeekMessageW(&message,nullptr,WM_APP,0xBFFF,PM_REMOVE)) {
            ::DispatchMessageW(&message);
            if(++posted>requests*4+100) break;
        }
        if(posted>requests*4+100) break;
        ::PeekMessageW(&message,nullptr,WM_NULL,WM_KEYFIRST-1,PM_NOREMOVE|PM_QS_SENDMESSAGE);
        RECT current{};CHECK(::GetClientRect(helper,&current));
        if(current.right!=previous.right || current.bottom!=previous.bottom) {
            ++observed_resizes;previous=current;
        }
        ::MsgWaitForMultipleObjects(0,nullptr,FALSE,5,QS_SENDMESSAGE|QS_POSTMESSAGE);
    }
    if(sentinel) ::KillTimer(nullptr,sentinel);
    std::printf("resize_without_timers: requests=%u observed=%u posted=%u timers=%u\n",
        requests,observed_resizes,posted,timer_dispatches);
    CHECK(timer_dispatches==0);
    CHECK(requests>=10 && observed_resizes>=3);
    CHECK(posted<=requests*4+100);
    CHECK(preview.snapshot().backend_process_id==before.backend_process_id);
    CHECK(preview.snapshot().player_generation==before.player_generation);
    CHECK(WaitFor([&] {
        RECT current{};::GetClientRect(helper,&current);
        return current.right==640+static_cast<int>(requests)*4 &&
            current.bottom==360+static_cast<int>(requests)*2;
    },3000));
}

void RunBackpressure(const std::wstring& source) {
    const RECT placement=easnap::test::OnTestMonitor({0,0,640,360});
    const HWND parent = ::CreateWindowExW(0, L"STATIC", L"Preview backpressure test",
        WS_POPUP | WS_CLIPCHILDREN, placement.left, placement.top, 640, 360, nullptr, nullptr,
        ::GetModuleHandleW(nullptr), nullptr);
    CHECK(parent != nullptr);
    if (parent == nullptr) return;

    auto preview = easnap::media::CreateMfPlayPreview();
    CHECK(preview != nullptr);
    if (preview == nullptr) {
        ::DestroyWindow(parent);
        return;
    }
    const bool opened = preview->Open(source, parent, parent, WM_APP + 1);
    CHECK(opened);
    const bool ready = opened && WaitFor([&] {
        return preview->snapshot().state == MediaPreviewState::Ready;
    });
    CHECK(ready);
    const bool requested = ready && preview->Play();
    CHECK(requested);
    const bool playing = requested && WaitFor([&] {
        const auto state = preview->snapshot();
        return state.native_playing && state.native_position_valid &&
            !state.command_pending && state.native_position > MediaTime{1'000'000};
    });
    CHECK(playing);

    if (playing) {
        const auto before = preview->snapshot();
        const HWND helper = ::GetWindow(parent, GW_CHILD);
        DWORD helper_process = 0;
        const DWORD helper_thread = ::GetWindowThreadProcessId(helper, &helper_process);
        const bool owned = helper != nullptr && helper_process != 0 &&
            helper_process != ::GetCurrentProcessId() &&
            helper_process == before.backend_process_id &&
            helper_thread == before.backend_thread_id;
        CHECK(owned);
        if (owned) {
            // This timer is a sentinel for the same posted-message category as
            // the production preview owner's private polling timer.
            timer_dispatches = 0;
            const UINT_PTR sentinel = ::SetTimer(nullptr, 0, 25, TimerDispatch);
            CHECK(sentinel != 0);
            const auto started = ::GetTickCount64();
            ProbeResult probe_result;
            std::thread probe(ProbeHelper, helper, started, std::ref(probe_result));
            bool resized = false;
            while (::GetTickCount64() - started < 3'600 && !probe_result.failed.load()) {
                MSG message{};
                // PeekMessage dispatches incoming sent messages itself. Do not
                // remove or dispatch WM_TIMER, including the preview Poll.
                ::PeekMessageW(&message, nullptr, WM_NULL, WM_KEYFIRST - 1,
                    PM_NOREMOVE | PM_QS_SENDMESSAGE);
                if (!resized && ::GetTickCount64() - started >= 600 &&
                    probe_result.replies.load() >= 2) {
                    // Only our parent is positioned. Production UpdateVideo
                    // must deliver resizing to the helper over its own channel.
                    CHECK(::SetWindowPos(parent, nullptr, 0, 0, 832, 468,
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW) != FALSE);
                    preview->UpdateVideo();
                    resized = true;
                }
                ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_SENDMESSAGE);
            }
            const auto starved_ms = ::GetTickCount64() - started;
            probe_result.stop.store(true);
            probe.join();
            CHECK(timer_dispatches == 0);
            CHECK(preview->snapshot().native_position == before.native_position);
            if (sentinel != 0) ::KillTimer(nullptr, sentinel);
            CHECK(!probe_result.failed.load());
            CHECK(starved_ms >= 3'600);
            CHECK(probe_result.replies.load() >= 20);
            CHECK(resized);

            RECT child_bounds{};
            CHECK(::GetClientRect(helper, &child_bounds) != FALSE);
            // The helper processed the resize while no new parent snapshots
            // were consumed, before normal polling is permitted to resume.
            CHECK(child_bounds.right == 832 && child_bounds.bottom == 468);
            std::fprintf(stdout,
                "parent_poll_starved_ms=%llu helper_replies=%u max_reply_ms=%llu "
                "failure_at_ms=%llu error=%lu child=%ldx%ld\n",
                starved_ms, probe_result.replies.load(), probe_result.longest_reply_ms.load(),
                probe_result.failure_at_ms.load(), probe_result.error.load(),
                child_bounds.right, child_bounds.bottom);

            const bool resumed = WaitFor([&] {
                const auto state = preview->snapshot();
                return state.backend_process_id == before.backend_process_id &&
                    state.player_generation == before.player_generation &&
                    state.native_playing && !state.command_pending &&
                    state.native_position > before.native_position + MediaTime{20'000'000};
            });
            CHECK(resumed);
            const auto after = preview->snapshot();
            CHECK(after.backend_process_id == before.backend_process_id);
            CHECK(after.player_generation == before.player_generation);
            CHECK(after.state != MediaPreviewState::Failed);
            CHECK(preview->Pause());
            CHECK(WaitFor([&] {
                const auto state = preview->snapshot();
                return state.state == MediaPreviewState::Paused &&
                    !state.native_playing && !state.command_pending;
            }));
            ResizeWhileTimersAreDeferred(*preview,parent,helper);
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "Preview diagnostics: %ls\n", preview->diagnostics().c_str());
    }
    preview->Close();
    preview.reset();
    ::DestroyWindow(parent);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CHECK(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2));
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    CHECK(SUCCEEDED(com));
    if (FAILED(com)) return 1;
    const HRESULT media = ::MFStartup(MF_VERSION);
    CHECK(SUCCEEDED(media));
    if (FAILED(media)) {
        ::CoUninitialize();
        return 1;
    }

    const auto root = argc > 1 ? std::filesystem::path(argv[1]) :
        std::filesystem::temp_directory_path() / L"easnap-preview-backpressure";
    const auto directory = root / (std::to_wstring(::GetCurrentProcessId()) + L"-" +
        std::to_wstring(::GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    CHECK(!error);
    const auto source = directory / L"preview.mp4";
    if (!error) {
        const bool created = easnap::test::CreateVideoPreviewSource(source.wstring());
        CHECK(created);
        if (created) RunBackpressure(source.wstring());
    }

    ::MFShutdown();
    ::CoUninitialize();
    if (failures == 0) {
        std::filesystem::remove(source, error);
        std::filesystem::remove(directory, error);
    } else {
        std::fprintf(stderr, "Fixture retained at %ls\n", source.c_str());
    }
    std::fprintf(stdout, "Preview backpressure failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
