// Easnap integration test - Scrollshot of a generated application on the real desktop.
//
// A second process of this executable shows the generated application of
// scrollshot_realistic_app.h - an application bar, text in five fonts with
// ClearType, pictures, a blinking caret, a spinner that changes every frame,
// pictures that load late, a list that grows at its end and an overlay
// scrollbar - in a flip-model swap chain, and scrolls it the way a browser
// does: each wheel notch or Page Down starts an eased animation presented once
// per refresh. It publishes the exact pixels of its content layer and its
// scroll state in a named file mapping.
//
// This process builds the production workspace over it as the application
// does - overlay, exposed frame, island, preview - and runs the production
// capture session. The session's provider is the production provider wrapped
// in an observer that passes every call through unchanged. The observer drops
// identical pictures with a restated copy of the session's filter (it is
// inline in the session's worker loop), finds each picture's true offset by
// an exact search in the published content layer and hands it to a second,
// shadow engine, which places it exactly as the session's engine does: so
// every placement can be checked, not only the final image.
//
// Takes the real pointer and the keyboard focus; runs on the visible desktop
// only (interactive-desktop). Input is sent only after the window that would
// receive it has been verified to be the application's.
// See docs/decisions/ADR-041-scrollshot-realistic-test-application.md.

#include "native_test_monitor.h"
#include "scrollshot_realistic_app.h"

#include "capture/capture_provider.h"
#include "scrollshot/scrollshot_session.h"
#include "scrollshot/stitch_engine.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/scrollshot_controls.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace easnap;
namespace realistic = easnap_test::realistic;
using Microsoft::WRL::ComPtr;

int passed{}, failed{};
#define CHECK(...)                                                         \
    do {                                                                   \
        if (__VA_ARGS__) {                                                 \
            ++passed;                                                      \
        } else {                                                           \
            ++failed;                                                      \
            std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); \
        }                                                                  \
    } while (false)

/// CTest reports this exit code as "skipped" (SKIP_RETURN_CODE).
constexpr int kSkipped = 77;
constexpr int kChildFailed = 3;

constexpr wchar_t kAppClass[] = L"EasnapScrollshotRealisticApp";
constexpr uint32_t kSeed = 4201;
constexpr LONG kSideMargin = 40;       ///< Application window beside the frame.
constexpr LONG kVerticalMargin = 140;  ///< Application window above and below the frame.

[[nodiscard]] uint32_t Rgb(int r, int g, int b) {
    return 0xFF000000u | (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

// --- The application: another process with a flip-model swap chain -----------------------

struct AppWindow {
    realistic::Simulation* simulation{nullptr};
    std::chrono::steady_clock::time_point start{};
    LONG inputs{0};
    [[nodiscard]] double Now() const {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};

LRESULT CALLBACK AppProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<AppWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_NCCREATE:
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            ::ValidateRect(window, nullptr);  // The swap chain presents every frame.
            return 0;
        case WM_MOUSEWHEEL:
            if (app != nullptr && app->simulation != nullptr) {
                app->simulation->Wheel(app->Now(), -GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA);
                ++app->inputs;
            }
            return 0;
        case WM_KEYDOWN:
            if (app != nullptr && app->simulation != nullptr && wparam == VK_NEXT) {
                app->simulation->PageDown(app->Now());
                ++app->inputs;
            }
            return 0;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

/// The application window's own chrome around the content pane: a tool bar
/// above it, a status bar below it and margins beside it. It never changes.
void DrawChrome(realistic::Pixels& window) {
    window.Reset(window.width, window.height, Rgb(240, 242, 245));
    const auto fill = [&](int left, int top, int right, int bottom, uint32_t colour) {
        for (int y = std::max(0, top); y < std::min(window.height, bottom); ++y) {
            for (int x = std::max(0, left); x < std::min(window.width, right); ++x) { window.Row(y)[x] = colour; }
        }
    };
    fill(0, 0, window.width, kVerticalMargin - 12, Rgb(250, 251, 252));
    fill(0, kVerticalMargin - 13, window.width, kVerticalMargin - 12, Rgb(214, 219, 225));
    for (int x = 16; x + 64 < window.width; x += 84) { fill(x, 24, x + 64, 52, Rgb(222, 226, 232)); }
    fill(0, window.height - kVerticalMargin + 12, window.width, window.height, Rgb(232, 235, 239));
}

/// `--app <left> <top> <width> <height>`: shows the application until closed.
int RunApp(int argc, wchar_t** argv) {
    if (argc != 6) { return 2; }
    const int left = _wtoi(argv[2]), top = _wtoi(argv[3]), width = _wtoi(argv[4]), height = _wtoi(argv[5]);
    if (width <= 2 * kSideMargin || height <= 2 * kVerticalMargin) { return 2; }

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = AppProc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kAppClass;
    if (::RegisterClassExW(&window_class) == 0) { return kChildFailed; }
    AppWindow state;
    // Above the windows already on the monitor, below the workspace that is
    // shown afterwards.
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kAppClass, L"Easnap Scrollshot realistic application",
                                    WS_POPUP, left, top, width, height, nullptr, nullptr, window_class.hInstance, &state);
    if (window == nullptr) { return kChildFailed; }

    realistic::AppOptions options;
    options.seed = kSeed;
    options.width = width - 2 * kSideMargin;
    options.view_height = height - 2 * kVerticalMargin;
    options.scale = static_cast<double>(::GetDpiForWindow(window)) / 96.0;
    options.document_rows = 2400;
    options.header = realistic::Header::Fixed;
    options.caret = true;
    const UINT blink = ::GetCaretBlinkTime();
    options.caret_blink_ms = blink == INFINITE ? 0 : static_cast<int>(blink);
    options.spinner = true;
    options.lazy_slots = 3;
    options.growing_tail = true;
    options.overlay_scrollbar = true;
    std::wstring error;
    const auto app = realistic::Application::Create(options, error);
    if (app == nullptr) {
        std::printf("The application could not be built: %ls\n", error.c_str());
        return realistic::UnavailableReason().empty() ? kChildFailed : kSkipped;
    }
    realistic::Simulation simulation(*app, kSeed);
    realistic::SharedPublisher shared;
    if (!shared.Create(realistic::SharedMappingName(::GetCurrentProcessId()), *app)) { return kChildFailed; }

    // A flip-model swap chain, as browsers and Direct2D applications present.
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                     0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    ComPtr<IDXGIDevice1> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> swap_chain;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Scaling = DXGI_SCALING_STRETCH;
    if (FAILED(hr) || FAILED(device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(device.Get(), window, &desc, nullptr, nullptr, &swap_chain))) {
        std::printf("The application process cannot create a Direct3D 11 flip-model swap chain.\n");
        return kSkipped;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    dxgi_device->SetMaximumFrameLatency(1);  // Present waits for the refresh: one frame per vsync.

    realistic::Pixels pixels;
    pixels.width = width;
    pixels.height = height;
    DrawChrome(pixels);
    realistic::Pixels view;
    state.simulation = &simulation;
    state.start = std::chrono::steady_clock::now();
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    LONG presented = 0;
    for (;;) {
        MSG message{};
        bool quit = false;
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            if (message.message == WM_QUIT) { quit = true; }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (quit) { break; }
        simulation.Present(state.Now());
        app->Compose(simulation.state(), view);
        for (int y = 0; y < view.height; ++y) {
            std::memcpy(pixels.Row(kVerticalMargin + y) + kSideMargin, view.Row(y),
                        static_cast<size_t>(view.width) * sizeof(uint32_t));
        }
        ComPtr<ID3D11Texture2D> back_buffer;
        if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) { return kChildFailed; }
        context->UpdateSubresource(back_buffer.Get(), 0, nullptr, pixels.data.data(),
                                   static_cast<UINT>(width) * sizeof(uint32_t), 0);
        const HRESULT shown = swap_chain->Present(1, 0);
        if (FAILED(shown)) { return kChildFailed; }
        if (shown == DXGI_STATUS_OCCLUDED) { ::DwmFlush(); }  // Pace by the compositor when nothing is shown.
        ++presented;
        shared.Publish(simulation, state.inputs, presented);
        if (presented == 2) { shared.SetReady(); }
    }
    return 0;
}

// --- The observer between the session and the production provider ------------------------

/// What happened to one picture the session examined.
struct Observed {
    int true_offset{-1};  ///< -1: the picture is nowhere in the published page.
    scrollshot::FrameResult result;
};

/// Runs on the session's worker thread; read only after the session stopped.
class Oracle {
public:
    explicit Oracle(const realistic::SharedReader& reader) : reader_(reader), header_(reader.header()) {}

    void Observe(const platform::BgraImage& frame, const capture::RegionCaptureResult& result, LONG loaded) {
        ++acquired;
        if (result.primary_failure.stage == capture::RegionCaptureFailureStage::None) {
            ++duplication;
        } else {
            ++fallback;
        }
        // The session examines a picture only when it differs from the last one
        // it examined. That filter is inline in the session's worker loop, so it
        // is restated here with the same comparison (whole-picture memcmp).
        if (frame.width == previous_.width && frame.height == previous_.height &&
            std::memcmp(frame.pixels.data(), previous_.pixels.data(), frame.pixels.size() * sizeof(uint32_t)) == 0) {
            ++identical;
            return;
        }
        // This work runs inline on the session's worker, between two of its
        // readings; its cost is measured and reported.
        const auto started = std::chrono::steady_clock::now();
        previous_ = frame;
        Observed observed;
        observed.true_offset = FindOffset(frame, loaded);
        observed.result = shadow_.Submit(
            scrollshot::FrameView{frame.pixels.data(), frame.width, frame.height, frame.width});
        if (observed.true_offset >= 0) { last_found_ = observed.true_offset; }
        observed_.push_back(observed);
        const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        observe_total_ms += spent;
        observe_worst_ms = std::max(observe_worst_ms, spent);
    }

    [[nodiscard]] const std::vector<Observed>& observed() const { return observed_; }
    /// The shadow engine's canvas, which must be the session's result.
    [[nodiscard]] int shadow_height() const { return shadow_.height(); }
    [[nodiscard]] const uint32_t* ShadowRow(int y) const { return shadow_.Row(y); }

    int acquired{0};
    int identical{0};
    int duplication{0};
    int fallback{0};
    double observe_total_ms{0};  ///< Time spent on examined pictures (copy, search, shadow engine).
    double observe_worst_ms{0};

    /// Compares one row outside the scrollbar columns and outside every box
    /// that covers document row `row`.
    [[nodiscard]] static bool RowMatches(const uint32_t* got, const uint32_t* want, int columns, int row,
                                         const realistic::Box* boxes, int count, const realistic::Box* more,
                                         int more_count) {
        std::array<std::pair<int, int>, 2 * realistic::SharedHeader::kMaxBoxes> spans{};
        int spans_used = 0;
        for (int i = 0; i < count; ++i) {
            if (boxes[i].HasRow(row)) { spans[static_cast<size_t>(spans_used++)] = {boxes[i].left, boxes[i].right}; }
        }
        for (int i = 0; i < more_count; ++i) {
            if (more[i].HasRow(row)) { spans[static_cast<size_t>(spans_used++)] = {more[i].left, more[i].right}; }
        }
        std::sort(spans.begin(), spans.begin() + spans_used);
        int x = 0;
        const auto same = [&](int from, int to) {
            for (int i = from; i < to; ++i) {
                if (((got[i] ^ want[i]) & 0x00FFFFFFu) != 0) { return false; }
            }
            return true;
        };
        for (int i = 0; i < spans_used; ++i) {
            if (!same(x, std::min(spans[static_cast<size_t>(i)].first, columns))) { return false; }
            x = std::max(x, spans[static_cast<size_t>(i)].second);
        }
        return same(x, columns);
    }

private:
    /// The picture at `offset`: the bar exactly, every loaded row outside the
    /// scrollbar, the caret, the spinner and the late pictures.
    [[nodiscard]] bool Matches(const platform::BgraImage& frame, int offset, LONG loaded) const {
        const int columns = header_.width - header_.scrollbar_columns;
        for (int y = 0; y < header_.header_rows; ++y) {
            if (!RowMatches(frame.row(y), reader_.HeaderRow(y), header_.width, -1, nullptr, 0, nullptr, 0)) { return false; }
        }
        // Middle rows first: they decide most candidates at once.
        for (int pass = 0; pass < 2; ++pass) {
            for (int y = header_.header_rows; y < frame.height; ++y) {
                const bool middle = y >= frame.height / 3 && y < 2 * frame.height / 3;
                if (middle != (pass == 0)) { continue; }
                const int row = offset + y;
                if (row >= loaded || row >= header_.content_height) { continue; }  // Loading row or not loaded yet.
                if (!RowMatches(frame.row(y), reader_.ContentRow(row), columns, row, header_.masks, header_.mask_count,
                                header_.slots, header_.slot_count)) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] int FindOffset(const platform::BgraImage& frame, LONG loaded) const {
        if (frame.width != header_.width || frame.height != header_.view_height) { return -1; }
        const int last = std::max(0, header_.content_height - header_.view_height);
        // Nearest to the last offset found first: the page moved a little.
        for (int distance = 0; distance <= last; ++distance) {
            const int below = last_found_ + distance, above = last_found_ - distance;
            if (below <= last && Matches(frame, below, loaded)) { return below; }
            if (distance > 0 && above >= 0 && Matches(frame, above, loaded)) { return above; }
            if (below > last && above < 0) { break; }
        }
        return -1;
    }

    const realistic::SharedReader& reader_;
    const realistic::SharedHeader& header_;
    scrollshot::StitchEngine shadow_;
    platform::BgraImage previous_;
    std::vector<Observed> observed_;
    int last_found_{0};
};

/// The production provider, unchanged, with the oracle looking on.
class ObservedProvider final : public capture::IRegionCaptureProvider {
public:
    ObservedProvider(std::unique_ptr<capture::IRegionCaptureProvider> inner, std::shared_ptr<Oracle> oracle,
                     const realistic::SharedReader& reader)
        : inner_(std::move(inner)), oracle_(std::move(oracle)), reader_(reader) {}

    bool CaptureDesktop(bool include_cursor, capture::DesktopSnapshot& out) override {
        return inner_->CaptureDesktop(include_cursor, out);
    }
    bool CaptureRegion(const RECT& region, bool include_cursor, platform::BgraImage& out) override {
        return inner_->CaptureRegion(region, include_cursor, out);
    }
    capture::RegionCaptureResult CaptureRegionFrame(const RECT& region, bool include_cursor, media::MediaTime timestamp,
                                                    media::MediaTime duration, media::VideoFrame& out) override {
        return inner_->CaptureRegionFrame(region, include_cursor, timestamp, duration, out);
    }
    capture::RegionCaptureResult CaptureRegionImage(const RECT& region, const capture::RegionImageOptions& options,
                                                    platform::BgraImage& out) override {
        // Rows below this were loaded before the picture was taken.
        const LONG loaded = realistic::ReadShared(reader_.header().loaded_height);
        const capture::RegionCaptureResult result = inner_->CaptureRegionImage(region, options, out);
        if (result.status == capture::RegionCaptureStatus::FrameAcquired && !out.empty()) {
            oracle_->Observe(out, result, loaded);
        }
        return result;
    }
    void Release() noexcept override { inner_->Release(); }
    [[nodiscard]] const wchar_t* name() const override { return inner_->name(); }

private:
    std::unique_ptr<capture::IRegionCaptureProvider> inner_;
    std::shared_ptr<Oracle> oracle_;
    const realistic::SharedReader& reader_;
};

// --- Driving it ---------------------------------------------------------------------------

/// Dispatches this thread's messages until `done` holds or `timeout` passes.
bool PumpUntil(const std::function<bool()>& done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        if (done()) { return true; }
        if (std::chrono::steady_clock::now() >= deadline) { return false; }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    }
}

void SendWheel(int notches) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(notches * WHEEL_DELTA);
    CHECK(::SendInput(1, &input, sizeof(input)) == 1);
}

void SendClick() {
    INPUT inputs[2]{};
    inputs[0].type = inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    CHECK(::SendInput(2, inputs, sizeof(INPUT)) == 2);
}

void SendKey(WORD key) {
    INPUT inputs[2]{};
    inputs[0].type = inputs[1].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = inputs[1].ki.wVk = key;
    inputs[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    inputs[1].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    CHECK(::SendInput(2, inputs, sizeof(INPUT)) == 2);
}

struct ChildProcess {
    HANDLE job{nullptr};
    PROCESS_INFORMATION info{};
    ~ChildProcess() {
        if (info.hProcess != nullptr) {
            if (::WaitForSingleObject(info.hProcess, 3000) != WAIT_OBJECT_0) { ::TerminateProcess(info.hProcess, 9); }
            ::CloseHandle(info.hThread);
            ::CloseHandle(info.hProcess);
        }
        if (job != nullptr) { ::CloseHandle(job); }  // Ends the application should this process die first.
    }
};

bool StartApp(const RECT& rect, ChildProcess& child) {
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) { return false; }
    wchar_t command[MAX_PATH + 96]{};
    std::swprintf(command, ARRAYSIZE(command), L"\"%ls\" --app %ld %ld %ld %ld", path, rect.left, rect.top,
                  rect.right - rect.left, rect.bottom - rect.top);
    child.job = ::CreateJobObjectW(nullptr, nullptr);
    if (child.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(path, command, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup,
                         &child.info) == FALSE) {
        return false;
    }
    if (child.job != nullptr) { ::AssignProcessToJobObject(child.job, child.info.hProcess); }
    ::ResumeThread(child.info.hThread);
    return true;
}

HWND FindApp(DWORD process) {
    for (HWND window = ::FindWindowExW(nullptr, nullptr, kAppClass, nullptr); window != nullptr;
         window = ::FindWindowExW(nullptr, window, kAppClass, nullptr)) {
        DWORD owner = 0;
        ::GetWindowThreadProcessId(window, &owner);
        if (owner == process) { return window; }
    }
    return nullptr;
}

int RunTest() {
    using namespace std::chrono_literals;
    const auto started = std::chrono::steady_clock::now();
    if (!ui::CaptureExclusionSupported()) {
        std::printf("SKIPPED: Scrollshot needs capture exclusion (Windows 10 version 2004 or later).\n");
        return kSkipped;
    }
    if (const std::wstring reason = realistic::UnavailableReason(); !reason.empty()) {
        std::printf("SKIPPED: the generated application cannot be rendered here: %ls.\n", reason.c_str());
        return kSkipped;
    }
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) {
        std::fprintf(stderr, "No monitor\n");
        return 1;
    }
    platform::MonitorInfo monitor = monitors.front();
    for (const auto& candidate : monitors) {
        if (candidate.primary) { monitor = candidate; }
    }
    if (const auto* selected = test::SelectedTestMonitor()) { monitor = *selected; }

    // The application window, with room around the frame for the border,
    // the island and the preview to lie over its own chrome.
    const LONG work_width = platform::RectWidth(monitor.work_area), work_height = platform::RectHeight(monitor.work_area);
    const LONG frame_width = std::clamp<LONG>(work_width - 400, 320, 720);
    const LONG frame_height = std::clamp<LONG>(work_height - 2 * kVerticalMargin - 60, 240, 480);
    const RECT app_rect{monitor.work_area.left + 60, monitor.work_area.top + 30,
                        monitor.work_area.left + 60 + frame_width + 2 * kSideMargin,
                        monitor.work_area.top + 30 + frame_height + 2 * kVerticalMargin};
    if (app_rect.right > monitor.work_area.right || app_rect.bottom > monitor.work_area.bottom) {
        std::printf("SKIPPED: the monitor's work area (%ld x %ld) is too small for the application.\n", work_width,
                    work_height);
        return kSkipped;
    }
    const RECT frame{app_rect.left + kSideMargin, app_rect.top + kVerticalMargin, app_rect.right - kSideMargin,
                     app_rect.bottom - kVerticalMargin};
    const POINT centre{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};

    ChildProcess child;
    CHECK(StartApp(app_rect, child));
    if (child.info.hProcess == nullptr) { return 1; }
    realistic::SharedReader reader;
    const std::wstring mapping = realistic::SharedMappingName(child.info.dwProcessId);
    const bool ready = PumpUntil([&] {
        return (reader.Open(mapping) && realistic::ReadShared(reader.header().presented) >= 2) ||
               ::WaitForSingleObject(child.info.hProcess, 0) == WAIT_OBJECT_0;
    },
                                 30s);
    DWORD child_exit = STILL_ACTIVE;
    ::GetExitCodeProcess(child.info.hProcess, &child_exit);
    if (child_exit == static_cast<DWORD>(kSkipped)) {
        std::printf("SKIPPED: the application process cannot present with a Direct3D 11 flip-model swap chain.\n");
        return kSkipped;
    }
    CHECK(ready && child_exit == STILL_ACTIVE);
    HWND app_window = FindApp(child.info.dwProcessId);
    CHECK(app_window != nullptr);
    if (!ready || child_exit != STILL_ACTIVE || app_window == nullptr) {
        std::fprintf(stderr, "  the application did not come up (exit code %lu)\n", child_exit);
        return 1;
    }
    const realistic::SharedHeader& shared = reader.header();
    const int width = shared.width, view = shared.view_height, header_rows = shared.header_rows;
    CHECK(width == static_cast<int>(frame_width) && view == static_cast<int>(frame_height));
    if (width != static_cast<int>(frame_width) || view != static_cast<int>(frame_height)) { return 1; }
    const auto value = [&](const volatile LONG& field) { return static_cast<int>(realistic::ReadShared(field)); };
    std::printf("Application %d x %d, page %d rows, bar %d rows, scrollbar %d columns, %d masks, %d late pictures\n",
                width, view, shared.content_height, header_rows, shared.scrollbar_columns, shared.mask_count,
                shared.slot_count);
    POINT pointer_before{};
    ::GetCursorPos(&pointer_before);

    // --- The workspace, built as the application builds it --------------------
    auto provider = capture::CreateRegionCaptureProvider();
    capture::DesktopSnapshot snapshot;
    CHECK(provider && provider->CaptureDesktop(false, snapshot));
    int cancels = 0, wheel_blocked = 0;
    ui::OverlayController::Callbacks overlay_callbacks;
    overlay_callbacks.on_cancelled = [&] { ++cancels; };
    overlay_callbacks.on_scrollshot_command = [&](ui::ScrollshotCommand command) {
        if (command == ui::ScrollshotCommand::WheelBlocked) { ++wheel_blocked; }
    };
    ui::OverlayController overlay(std::move(overlay_callbacks));
    CHECK(overlay.Show(snapshot, centre));
    overlay.EnterEditing();
    overlay.selector().SetSelection(frame);
    overlay.RegionChanged();
    // Whether this desktop can be duplicated at all, asked while the workspace
    // that has just appeared still gives the duplication a new frame.
    capture::RegionImageOptions probe_options;
    platform::BgraImage probe;
    const capture::RegionCaptureResult probed = provider->CaptureRegionImage(app_rect, probe_options, probe);
    const bool duplication_available = probed.status == capture::RegionCaptureStatus::FrameAcquired &&
                                       probed.primary_failure.stage == capture::RegionCaptureFailureStage::None;

    overlay.SetMode(app::CaptureMode::Scrollshot);
    ui::ScrollshotControls::Callbacks control_callbacks;
    control_callbacks.on_wheel_blocked = [&] { ++wheel_blocked; };
    ui::ScrollshotControls controls(std::move(control_callbacks));
    CHECK(controls.Show(ui::ScrollshotControls::View::Start, frame, overlay.panel_owner_window()));
    CHECK(ui::CanCaptureVisibleWindows());  // What the application requires before it starts.

    scrollshot::ScrollshotSession::Options options;
    options.region = frame;
    options.generation = 1;
    options.preview_width = ui::ScrollshotControls::PreviewPixelWidth(frame);
    // As the application does before the first picture: it gives up its own
    // duplication of the output and rearranges the shading.
    provider->Release();
    overlay.ExposeFrame(true);
    auto oracle = std::make_shared<Oracle>(reader);
    scrollshot::ScrollshotSession session(
        std::make_unique<ObservedProvider>(capture::CreateRegionCaptureProvider(), oracle, reader), options);
    const bool session_started = session.Start();
    CHECK(session_started);
    int offset = 0;
    std::set<int> rest_offsets{0};
    if (session_started) {
        overlay.BeginScrollshotCapture();
        CHECK(controls.Show(ui::ScrollshotControls::View::Capture, frame, overlay.panel_owner_window()));
        scrollshot::ScrollshotPreview preview;
        session.CopyPreview(4096, preview);
        controls.SetPreview(preview);
        CHECK(ui::CanCaptureVisibleWindows());

        // --- Real input -------------------------------------------------------
        bool following = true;
        // The page came to rest where it was sent, its late pictures loaded,
        // and the session stitched the rows that came into view.
        const auto settled = [&]() {
            const bool reached = PumpUntil([&] {
                const int target = value(shared.target);
                return value(shared.offset) == target && value(shared.animating) == 0 &&
                       value(shared.pending_lazy) == 0 && session.progress().height >= view + target;
            },
                                           6s);
            offset = value(shared.offset);
            if (!reached) {
                std::fprintf(stderr, "  page at %d (target %d, animating %d, %d pictures loading), %d rows stitched, last outcome %ls\n",
                             offset, value(shared.target), value(shared.animating), value(shared.pending_lazy),
                             session.progress().height, scrollshot::ToString(session.progress().last_outcome));
            } else {
                rest_offsets.insert(offset);
            }
            return reached;
        };
        const auto received = [&](int before) {
            return PumpUntil([&] { return value(shared.inputs) > before; }, 3s);
        };
        // Never turn the wheel over a window that is not the application.
        const auto wheel = [&](int notches_in_a_row) {
            for (int i = 0; i < notches_in_a_row && following; ++i) {
                if (::WindowFromPoint(centre) != app_window) {
                    following = false;
                    break;
                }
                const int before = value(shared.inputs);
                SendWheel(-1);
                following = received(before);
            }
            following = following && settled();
        };

        ::SetCursorPos(centre.x, centre.y);
        // The frame is open: the pointer over it is over the other application.
        CHECK(PumpUntil([&] { POINT at{}; return ::GetCursorPos(&at) && at.x == centre.x && at.y == centre.y; }, 2s));
        CHECK(::WindowFromPoint(centre) == app_window);
        const bool by_focus = platform::WheelFollowsFocus();
        if (by_focus) {
            // Windows sends the wheel to the active window only. The documented
            // way on: click inside the frame once - and only into the application.
            std::printf("Wheel routing follows the focus on this computer: clicking inside the frame first.\n");
            if (::WindowFromPoint(centre) == app_window) {
                SendClick();
                CHECK(PumpUntil([&] { return ::GetForegroundWindow() == app_window; }, 3s));
                following = ::GetForegroundWindow() == app_window;
            } else {
                following = false;
            }
        }
        for (int i = 0; i < 6; ++i) { wheel(1); }  // One notch at a time.
        for (int i = 0; i < 3; ++i) { wheel(2); }  // The second notch arrives mid-animation.
        CHECK(following);
        if (!by_focus) { CHECK(wheel_blocked == 0); }

        // A click inside the frame gives the application the keyboard; the
        // workspace neither cancels nor starts a new selection.
        if (following && ::WindowFromPoint(centre) == app_window) {
            SendClick();
            const bool focused = PumpUntil([&] { return ::GetForegroundWindow() == app_window; }, 3s);
            CHECK(focused);
            const RECT selection = overlay.selector().selection();
            CHECK(::EqualRect(&selection, &frame) && cancels == 0 && overlay.workspace().scrollshot_capturing());
            for (int i = 0; i < 2 && focused && following; ++i) {
                if (::GetForegroundWindow() != app_window) {  // Keys only to the application.
                    following = false;
                    break;
                }
                const int before = value(shared.inputs);
                SendKey(VK_NEXT);
                following = received(before) && settled();
            }
            CHECK(following);
        }
        // On to the end of the list, which loads more rows twice on the way.
        for (int i = 0; i < 80 && following; ++i) {
            if (value(shared.complete) != 0 && value(shared.loading) == 0 && offset == value(shared.max_scroll)) { break; }
            wheel(1);
        }
        CHECK(following && value(shared.complete) != 0 && offset == value(shared.max_scroll));

        session.CopyPreview(4096, preview);
        controls.SetPreview(preview);
        CHECK(preview.width == options.preview_width && preview.height > 0);
        const scrollshot::ScrollshotProgress progress = session.progress();
        CHECK(progress.condition == scrollshot::ScrollshotCondition::Following);
        if (duplication_available) { CHECK(progress.fallback_frames <= 3); }

        const bool stopped = session.Stop(5s);
        CHECK(stopped);
        overlay.StopScrollshotCapture();
        CHECK(overlay.workspace().scrollshot_locked() && cancels == 0);
        if (stopped) {
            // --- Every picture the session examined ------------------------------
            int written = 0, refreshed = 0, unchanged = 0, ambiguous = 0, low_information = 0, no_overlap = 0;
            int unmatched = 0, unmatched_written = 0, misplaced = 0, mid_animation = 0, shadow_placed = 0;
            const std::vector<Observed>& observed = oracle->observed();
            const int origin = observed.empty() ? -1 : observed.front().true_offset;
            CHECK(!observed.empty() && observed.front().result.outcome == scrollshot::FrameOutcome::First);
            CHECK(origin == 0);
            for (size_t i = 0; i < observed.size(); ++i) {
                const Observed& entry = observed[i];
                const scrollshot::FrameResult& result = entry.result;
                if (entry.true_offset < 0) { ++unmatched; }
                if (entry.true_offset >= 0 && rest_offsets.count(entry.true_offset) == 0) { ++mid_animation; }
                if (result.outcome == scrollshot::FrameOutcome::Unchanged) {
                    ++unchanged;
                } else if (result.placed) {
                    ++written;
                    // Counted as the session counts its placed pictures.
                    if (result.position >= 0) { ++shadow_placed; }
                    if (result.outcome == scrollshot::FrameOutcome::Refreshed) { ++refreshed; }
                    if (entry.true_offset < 0) {
                        ++unmatched_written;
                    } else if (result.position != entry.true_offset - origin) {
                        if (misplaced < 4) {
                            std::fprintf(stderr, "  picture %zu: %ls at %d, true offset %d\n", i,
                                         scrollshot::ToString(result.outcome), result.position, entry.true_offset - origin);
                        }
                        ++misplaced;
                    }
                } else if (result.outcome == scrollshot::FrameOutcome::Ambiguous) {
                    ++ambiguous;
                } else if (result.outcome == scrollshot::FrameOutcome::LowInformation) {
                    ++low_information;
                } else if (result.outcome == scrollshot::FrameOutcome::NoOverlap) {
                    ++no_overlap;
                }
            }

            // --- The stitched image against the page -----------------------------
            const platform::BgraImage result = session.TakeResult();
            const int expected_height = view + offset - std::max(origin, 0);
            int differing = 0, first_differing = -1, scrollbar_rows = 0, bar_rows = 0;
            const int columns = width - shared.scrollbar_columns;
            CHECK(result.width == width);
            CHECK(result.height == expected_height);
            if (result.width == width && origin >= 0) {
                for (int row = 0; row < std::min(result.height, expected_height); ++row) {
                    if (row < header_rows) {
                        if (!Oracle::RowMatches(result.row(row), reader.HeaderRow(row), width, -1, nullptr, 0, nullptr, 0)) {
                            ++bar_rows;
                        }
                        continue;
                    }
                    const int document_row = origin + row;
                    const uint32_t* expected = reader.ContentRow(document_row);
                    if (expected == nullptr) {
                        ++differing;
                        continue;
                    }
                    if (!Oracle::RowMatches(result.row(row), expected, columns, document_row, shared.masks,
                                            shared.mask_count, nullptr, 0)) {
                        if (differing == 0) { first_differing = row; }
                        ++differing;
                    }
                    if (!Oracle::RowMatches(result.row(row) + columns, expected + columns, width - columns, -1, nullptr, 0,
                                            nullptr, 0)) {
                        ++scrollbar_rows;
                    }
                }
            }
            if (differing != 0) { std::fprintf(stderr, "  %d of %d rows differ, first %d\n", differing, result.height, first_differing); }

            // The shadow engine saw what the session's engine saw: the same
            // canvas, pixel for pixel, and the same number of placed pictures.
            const scrollshot::ScrollshotProgress finished = session.progress();
            int shadow_rows_unlike = 0;
            const int shadow_height = oracle->shadow_height();
            if (shadow_height == result.height && result.width == width) {
                for (int row = 0; row < result.height; ++row) {
                    const uint32_t* shadow_row = oracle->ShadowRow(row);
                    if (shadow_row == nullptr ||
                        !Oracle::RowMatches(result.row(row), shadow_row, width, -1, nullptr, 0, nullptr, 0)) {
                        ++shadow_rows_unlike;
                    }
                }
            }

            // The lab reads the capture path from this line.
            std::printf("EASNAP_CAPTURE_PATH=%s (%d pictures through Desktop Duplication, %d through GDI)\n",
                        oracle->duplication > 0 ? "Duplication" : "GDI", oracle->duplication, oracle->fallback);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            std::printf("MEASURE scrollshot_realistic_desktop frames=%d identical=%d submitted=%zu mid_animation=%d "
                        "written=%d refreshed=%d unchanged=%d refused=%d (ambiguous=%d low_information=%d "
                        "no_overlap=%d) unmatched=%d unmatched_written=%d misplaced=%d fallback_frames=%u height=%d "
                        "expected_height=%d differing_rows=%d first_differing=%d bar_rows_differing=%d "
                        "scrollbar_rows=%d shadow_height=%d shadow_rows_unlike=%d session_placed=%u shadow_placed=%d "
                        "observe_ms_mean=%.2f observe_ms_worst=%.2f inputs=%d rest_offsets=%zu "
                        "duplication_available=%d seconds=%.1f\n",
                        oracle->acquired, oracle->identical, observed.size(), mid_animation, written, refreshed,
                        unchanged, ambiguous + low_information + no_overlap, ambiguous, low_information, no_overlap,
                        unmatched, unmatched_written, misplaced, progress.fallback_frames, result.height,
                        expected_height, differing, first_differing, bar_rows, scrollbar_rows, shadow_height,
                        shadow_rows_unlike, finished.placed_frames, shadow_placed,
                        observed.empty() ? 0.0 : oracle->observe_total_ms / static_cast<double>(observed.size()),
                        oracle->observe_worst_ms, value(shared.inputs),
                        rest_offsets.size(), duplication_available ? 1 : 0, seconds);
            CHECK(unmatched_written == 0);  // Nothing of Easnap, no pointer, no torn picture was stitched.
            CHECK(misplaced == 0);
            CHECK(differing == 0);
            CHECK(bar_rows == 0);
            CHECK(shadow_height == result.height);
            CHECK(shadow_rows_unlike == 0);
            CHECK(finished.placed_frames == static_cast<uint32_t>(shadow_placed));
        }
    }

    controls.Destroy();
    overlay.Destroy();
    ::SetCursorPos(pointer_before.x, pointer_before.y);
    ::PostMessageW(app_window, WM_CLOSE, 0, 0);
    CHECK(::WaitForSingleObject(child.info.hProcess, 5000) == WAIT_OBJECT_0);
    std::printf("scrollshot realistic desktop: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc >= 2 && std::wcscmp(argv[1], L"--app") == 0) { return RunApp(argc, argv); }
    // Assertions and the log name things in English, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        std::fprintf(stderr, "FAIL: CoInitializeEx returned 0x%08lX\n", static_cast<unsigned long>(com));
        return 1;
    }
    int result = 1;
    try {
        result = RunTest();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
    }
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();  // Balances the successful CoInitializeEx above.
    return result;
}
