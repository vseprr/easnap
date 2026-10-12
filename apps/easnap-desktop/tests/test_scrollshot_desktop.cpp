// Easnap integration test - Scrollshot against the real desktop.
//
// A second process of this executable shows a known tall document, one screen
// pixel per document pixel, and scrolls it when it receives the wheel or Page
// Down. This process builds the production workspace over it in the order the
// application does - overlay, locked frame, island, preview - and runs the
// production capture session with the production provider.
//
// The content is scrolled with real input: the wheel turned over the frame has
// to pass through Easnap's input plane into the other process, and a click in
// the frame has to give that process the keyboard. What the session returns
// must be the document: nothing of Easnap, no mouse pointer, no row missing or
// repeated.
//
// Takes the real pointer and the keyboard focus. Input is sent only after the
// window that would receive it has been verified to be the test page.

#include "native_test_monitor.h"
#include "scrollshot_synthetic.h"

#include "capture/capture_provider.h"
#include "scrollshot/scrollshot_session.h"
#include "strings/strings.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/scrollshot_controls.h"

#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

namespace {

using namespace easnap;
using easnap_test::synthetic::Image;
using easnap_test::synthetic::MakeArticle;

int passed{}, failed{};
#define CHECK(...) do { if (__VA_ARGS__) { ++passed; } else { ++failed; std::fprintf(stderr, "FAIL %d: %s\n", __LINE__, #__VA_ARGS__); } } while (false)

/// CTest reports this exit code as "skipped" (SKIP_RETURN_CODE).
constexpr int kSkipped = 77;

constexpr wchar_t kPageClass[] = L"EasnapScrollshotTestPage";
constexpr wchar_t kOffsetProperty[] = L"EasnapTestPageOffset";  ///< Top document row + 1; absent until painted.
constexpr wchar_t kInputProperty[] = L"EasnapTestPageInputs";   ///< Wheel and key messages handled.
constexpr uint32_t kSeed = 4117;
constexpr int kWheelRows = 120;  ///< Rows the page scrolls per wheel notch.
constexpr int kPageRows = 200;   ///< Rows the page scrolls per Page Down.
constexpr LONG kSideMargin = 40;       ///< Page shown beside the frame.
constexpr LONG kVerticalMargin = 140;  ///< Page shown above and below the frame.
constexpr int kScrollRange = 2400;     ///< Rows the page can scroll in total.

// --- The page: another application's scrolling window -----------------------------

struct Page {
    Image document;
    int offset{0};
    int inputs{0};
    int client_height{0};
};

void ScrollPage(HWND window, Page& page, int rows) {
    page.offset = std::clamp(page.offset + rows, 0, page.document.height - page.client_height);
    ++page.inputs;
    ::SetPropW(window, kInputProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(page.inputs)));
    ::InvalidateRect(window, nullptr, FALSE);
    ::UpdateWindow(window);
}

LRESULT CALLBACK PageProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* page = reinterpret_cast<Page*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_NCCREATE:
        ::SetWindowLongPtrW(window, GWLP_USERDATA,
                            reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams));
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        if (page != nullptr) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = page->document.width;
            info.bmiHeader.biHeight = -page->client_height;  // Top-down.
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            ::SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(page->document.width), static_cast<DWORD>(page->client_height),
                                0, 0, 0, static_cast<UINT>(page->client_height), page->document.Row(page->offset), &info,
                                DIB_RGB_COLORS);
            ::SetPropW(window, kOffsetProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(page->offset + 1)));
        }
        ::EndPaint(window, &paint);
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (page != nullptr) { ScrollPage(window, *page, -GET_WHEEL_DELTA_WPARAM(wparam) * kWheelRows / WHEEL_DELTA); }
        return 0;
    case WM_KEYDOWN:
        if (page != nullptr && wparam == VK_NEXT) { ScrollPage(window, *page, kPageRows); }
        return 0;
    case WM_DESTROY:
        ::RemovePropW(window, kOffsetProperty);
        ::RemovePropW(window, kInputProperty);
        ::PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

/// `--page <left> <top> <width> <height>`: shows the document until closed.
int RunPage(int argc, wchar_t** argv) {
    if (argc != 6) { return 2; }
    const int left = _wtoi(argv[2]), top = _wtoi(argv[3]), width = _wtoi(argv[4]), height = _wtoi(argv[5]);
    if (width <= 0 || height <= 0) { return 2; }
    Page page;
    page.document = MakeArticle(width, height + kScrollRange, kSeed);
    page.client_height = height;

    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = PageProc;
    window_class.hInstance = ::GetModuleHandleW(nullptr);
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.lpszClassName = kPageClass;
    if (::RegisterClassExW(&window_class) == 0) { return 3; }
    // Above the windows already on the monitor, below the workspace that is
    // shown afterwards.
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kPageClass, L"Easnap Scrollshot test page",
                                    WS_POPUP, left, top, width, height, nullptr, nullptr, window_class.hInstance, &page);
    if (window == nullptr) { return 3; }
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(window);
    MSG message{};
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return 0;
}

// --- Driving it -----------------------------------------------------------------------

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

int PageValue(HWND page, const wchar_t* property) {
    return static_cast<int>(reinterpret_cast<INT_PTR>(::GetPropW(page, property)));
}

bool Overlaps(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
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

/// Rows of `image` that are not the document rectangle starting at (`left`,
/// `top`). `shifted` counts pixels whose channels are one level off, which
/// does not make a row differ: under a translucent capture-excluded window
/// Windows returns a few values changed by one (measured here: 47 as 46 and
/// 70 as 69). The frame itself must not show that; see where it is checked.
int DifferingRows(const platform::BgraImage& image, const Image& document, int left, int top, int* first = nullptr,
                  int* shifted = nullptr) {
    int differing = 0;
    for (int y = 0; y < image.height; ++y) {
        const uint32_t* expected = document.Row(top + y) + left;
        bool equal = true;
        for (int x = 0; x < image.width; ++x) {
            const uint32_t got = image.row(y)[x] & 0x00ffffffu, want = expected[x] & 0x00ffffffu;
            if (got == want) { continue; }
            int largest = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                largest = std::max(largest, std::abs(static_cast<int>((got >> shift) & 0xff) - static_cast<int>((want >> shift) & 0xff)));
            }
            if (largest > 1) { equal = false; break; }
            if (shifted != nullptr) { ++*shifted; }
        }
        if (!equal) {
            if (differing == 0 && first != nullptr) { *first = y; }
            ++differing;
        }
    }
    return differing;
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
        if (job != nullptr) { ::CloseHandle(job); }  // Ends the page should this process die first.
    }
};

bool StartPage(const RECT& rect, ChildProcess& child) {
    wchar_t path[MAX_PATH]{};
    if (::GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)) == 0) { return false; }
    wchar_t command[MAX_PATH + 96]{};
    std::swprintf(command, ARRAYSIZE(command), L"\"%ls\" --page %ld %ld %ld %ld", path, rect.left, rect.top,
                  rect.right - rect.left, rect.bottom - rect.top);
    child.job = ::CreateJobObjectW(nullptr, nullptr);
    if (child.job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(child.job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    if (::CreateProcessW(path, command, nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &child.info) == FALSE) {
        return false;
    }
    if (child.job != nullptr) { ::AssignProcessToJobObject(child.job, child.info.hProcess); }
    ::ResumeThread(child.info.hThread);
    return true;
}

HWND FindPage(DWORD process) {
    for (HWND window = ::FindWindowExW(nullptr, nullptr, kPageClass, nullptr); window != nullptr;
         window = ::FindWindowExW(nullptr, window, kPageClass, nullptr)) {
        DWORD owner = 0;
        ::GetWindowThreadProcessId(window, &owner);
        if (owner == process) { return window; }
    }
    return nullptr;
}

int RunTest() {
    using namespace std::chrono_literals;
    if (!ui::CaptureExclusionSupported()) {
        std::printf("SKIPPED: Scrollshot needs capture exclusion (Windows 10 version 2004 or later).\n");
        return kSkipped;
    }
    const auto monitors = platform::EnumerateMonitors();
    if (monitors.empty()) { std::fprintf(stderr, "No monitor\n"); return 1; }
    platform::MonitorInfo monitor = monitors.front();
    for (const auto& candidate : monitors) { if (candidate.primary) { monitor = candidate; } }
    if (const auto* selected = test::SelectedTestMonitor()) { monitor = *selected; }

    // The page, with room around the frame for the border, the island and the
    // preview to lie over known pixels.
    const LONG work_width = platform::RectWidth(monitor.work_area), work_height = platform::RectHeight(monitor.work_area);
    const LONG frame_width = std::clamp<LONG>(work_width - 400, 320, 720);
    const LONG frame_height = std::clamp<LONG>(work_height - 2 * kVerticalMargin - 60, 240, 480);
    const RECT page_rect{monitor.work_area.left + 60, monitor.work_area.top + 30,
                         monitor.work_area.left + 60 + frame_width + 2 * kSideMargin,
                         monitor.work_area.top + 30 + frame_height + 2 * kVerticalMargin};
    if (page_rect.right > monitor.work_area.right || page_rect.bottom > monitor.work_area.bottom) {
        std::printf("SKIPPED: the monitor's work area (%ld x %ld) is too small for the test page.\n", work_width, work_height);
        return kSkipped;
    }
    const RECT frame{page_rect.left + kSideMargin, page_rect.top + kVerticalMargin, page_rect.right - kSideMargin,
                     page_rect.bottom - kVerticalMargin};
    const POINT centre{(frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2};
    const int page_width = static_cast<int>(platform::RectWidth(page_rect));
    const int page_height = static_cast<int>(platform::RectHeight(page_rect));
    const Image document = MakeArticle(page_width, page_height + kScrollRange, kSeed);

    ChildProcess child;
    CHECK(StartPage(page_rect, child));
    HWND page = nullptr;
    CHECK(PumpUntil([&] { page = FindPage(child.info.dwProcessId); return page != nullptr && PageValue(page, kOffsetProperty) == 1; }, 10s));
    if (page == nullptr) { return 1; }
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
    // As the application does before the first picture.
    overlay.ExposeScrollshotFrame(true);
    scrollshot::ScrollshotSession session(capture::CreateRegionCaptureProvider(), options);
    const bool started = session.Start();
    CHECK(started);
    if (started) {
        overlay.BeginScrollshotCapture();
        CHECK(controls.Show(ui::ScrollshotControls::View::Capture, frame, overlay.panel_owner_window()));
        scrollshot::ScrollshotPreview preview;
        session.CopyPreview(4096, preview);
        controls.SetPreview(preview);
        CHECK(ui::CanCaptureVisibleWindows());

        // Easnap is on screen over the page - the frame's border around the
        // frame, the island and the preview beside it - and none of it is in
        // a picture of that part of the screen.
        RECT island{}, preview_rect{};
        ::GetWindowRect(controls.hwnd(), &island);
        ::GetWindowRect(controls.preview_hwnd(), &preview_rect);
        CHECK(::IsWindowVisible(controls.hwnd()) && Overlaps(island, page_rect) && !Overlaps(island, frame));
        CHECK(::IsWindowVisible(controls.preview_hwnd()) && !Overlaps(preview_rect, frame));
        std::printf("Island %ld,%ld-%ld,%ld and preview %ld,%ld-%ld,%ld over page %ld,%ld-%ld,%ld (%ls)\n",
                    island.left, island.top, island.right, island.bottom, preview_rect.left, preview_rect.top,
                    preview_rect.right, preview_rect.bottom, page_rect.left, page_rect.top, page_rect.right,
                    page_rect.bottom, provider->name());
        platform::BgraImage around;
        int around_first = -1, around_differing = -1, around_shifted = 0, frame_shifted = 0;
        capture::RegionImageOptions image_options;
        image_options.require_every_output = true;
        const bool clean = PumpUntil([&] {
            ::DwmFlush();
            const auto result = provider->CaptureRegionImage(page_rect, image_options, around);
            if (result.status == capture::RegionCaptureStatus::Failed || around.empty()) { return false; }
            around_shifted = 0;
            around_differing = DifferingRows(around, document, 0, 0, &around_first, &around_shifted);
            if (around_differing != 0) { return false; }
            // The frame alone: no translucent window contains it, so it is exact.
            platform::BgraImage inner;
            inner.Reset(static_cast<int>(frame_width), static_cast<int>(frame_height));
            for (int y = 0; y < inner.height; ++y) {
                std::copy(around.row(y + kVerticalMargin) + kSideMargin,
                          around.row(y + kVerticalMargin) + kSideMargin + inner.width, inner.row(y));
            }
            frame_shifted = 0;
            return DifferingRows(inner, document, kSideMargin, kVerticalMargin, nullptr, &frame_shifted) == 0;
        }, 3s);
        if (!clean) { std::fprintf(stderr, "  %d rows around the frame differ, first %d\n", around_differing, around_first); }
        CHECK(clean);
        CHECK(frame_shifted == 0);

        // --- Real input -------------------------------------------------------
        int offset = 0, inputs = 0;
        bool following = true;
        const auto scrolled_to = [&](int expected) {
            // The page moved and the session stitched the rows that came into view.
            const bool reached = PumpUntil([&] {
                return PageValue(page, kOffsetProperty) == expected + 1 &&
                       session.progress().height >= static_cast<int>(frame_height) + expected;
            }, 4s);
            if (!reached) {
                std::fprintf(stderr, "  expected offset %d: page at %d, %d rows stitched, last outcome %ls\n", expected,
                             PageValue(page, kOffsetProperty) - 1, session.progress().height,
                             scrollshot::ToString(session.progress().last_outcome));
            }
            offset = PageValue(page, kOffsetProperty) - 1;
            inputs = PageValue(page, kInputProperty);
            return reached;
        };
        const auto wheel = [&](int notches) {
            // Never turn the wheel over a window that is not the test page.
            if (!following || ::WindowFromPoint(centre) != page) { following = false; return; }
            const int expected = std::min(offset + notches * kWheelRows, kScrollRange);
            SendWheel(-notches);
            following = scrolled_to(expected);
        };

        ::SetCursorPos(centre.x, centre.y);
        // The frame is open: the pointer over it is over the other application.
        CHECK(PumpUntil([&] { POINT at{}; return ::GetCursorPos(&at) && at.x == centre.x && at.y == centre.y; }, 2s));
        CHECK(::WindowFromPoint(centre) == page);
        CHECK(::WindowFromPoint(POINT{frame.left, frame.top}) == page);
        CHECK(::WindowFromPoint(POINT{frame.right - 1, frame.bottom - 1}) == page);
        // Outside it Easnap takes the input, so nothing else can be clicked by accident.
        DWORD plane_process = 0;
        ::GetWindowThreadProcessId(::WindowFromPoint(POINT{frame.left - 20, centre.y}), &plane_process);
        CHECK(plane_process == ::GetCurrentProcessId());

        const bool by_focus = platform::WheelFollowsFocus();
        if (by_focus) {
            // Windows sends the wheel to the active window only. The documented
            // way on: click inside the frame once.
            std::printf("Wheel routing follows the focus on this computer: clicking inside the frame first.\n");
            SendClick();
            CHECK(PumpUntil([&] { return ::GetForegroundWindow() == page; }, 3s));
            following = ::GetForegroundWindow() == page;
        }
        for (int i = 0; i < 6; ++i) { wheel(1); }
        CHECK(following && offset == 6 * kWheelRows);
        for (int i = 0; i < 3; ++i) { wheel(2); }  // Half a frame at a time.
        CHECK(following && offset == 12 * kWheelRows);
        if (!by_focus) { CHECK(wheel_blocked == 0); }

        // A click inside the frame goes to the content and gives it the
        // keyboard; the workspace neither cancels nor starts a new selection.
        if (following && ::WindowFromPoint(centre) == page) {
            SendClick();
            const bool focused = PumpUntil([&] { return ::GetForegroundWindow() == page; }, 3s);
            CHECK(focused);
            const RECT selection = overlay.selector().selection();
            CHECK(::EqualRect(&selection, &frame) && cancels == 0 && overlay.workspace().scrollshot_capturing());
            for (int i = 0; i < 2 && focused && following; ++i) {
                if (::GetForegroundWindow() != page) { following = false; break; }  // Keys only to the page.
                const int expected = std::min(offset + kPageRows, kScrollRange);
                SendKey(VK_NEXT);
                following = scrolled_to(expected);
            }
            CHECK(following && offset == 12 * kWheelRows + 2 * kPageRows);
        }
        // On to the end of the page; the last step is a short one.
        for (int i = 0; i < 16 && following && offset < kScrollRange; ++i) { wheel(1); }
        CHECK(following && offset == kScrollRange);
        // At the end the wheel moves nothing and nothing is added.
        if (following && ::WindowFromPoint(centre) == page) {
            const int before = inputs;
            SendWheel(-1);
            CHECK(PumpUntil([&] { return PageValue(page, kInputProperty) > before; }, 3s));
            ::DwmFlush();
        }

        session.CopyPreview(4096, preview);
        controls.SetPreview(preview);
        CHECK(preview.width == options.preview_width && preview.height > 0);
        const scrollshot::ScrollshotProgress progress = session.progress();
        std::printf("Scrolled %d rows with %d inputs: %d x %d stitched from %u pictures, %ls\n", offset, inputs,
                    progress.width, progress.height, progress.placed_frames, scrollshot::ToString(progress.last_outcome));
        CHECK(progress.condition == scrollshot::ScrollshotCondition::Following);

        const bool stopped = session.Stop(5s);
        CHECK(stopped);
        overlay.StopScrollshotCapture();
        CHECK(overlay.workspace().scrollshot_locked() && cancels == 0);
        if (stopped) {
            const platform::BgraImage result = session.TakeResult();
            CHECK(result.width == static_cast<int>(frame_width));
            CHECK(result.height == static_cast<int>(frame_height) + offset);
            if (result.width == static_cast<int>(frame_width) && result.height > 0 &&
                result.height <= document.height - static_cast<int>(kVerticalMargin)) {
                int first = -1, shifted = 0;
                const int differing = DifferingRows(result, document, kSideMargin, kVerticalMargin, &first, &shifted);
                if (differing != 0) { std::fprintf(stderr, "  %d of %d rows differ, first %d\n", differing, result.height, first); }
                std::printf("Stitched image against the document: %d rows differ, %d of %zu pixels one level off "
                            "(%d under the shading around the frame)\n", differing, shifted, result.pixels.size(), around_shifted);
                CHECK(shifted == 0);  // Exactly the pixels the other application drew.
                CHECK(differing == 0);
            }
        }
    }

    controls.Destroy();
    overlay.Destroy();
    ::SetCursorPos(pointer_before.x, pointer_before.y);
    ::PostMessageW(page, WM_CLOSE, 0, 0);
    CHECK(::WaitForSingleObject(child.info.hProcess, 5000) == WAIT_OBJECT_0);
    std::printf("scrollshot desktop: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc >= 2 && std::wcscmp(argv[1], L"--page") == 0) { return RunPage(argc, argv); }
    // Assertions and the log name things in English, whatever Windows speaks.
    easnap::strings::SetPreference(easnap::strings::LanguagePreference::English);
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 1;
    try {
        result = RunTest();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
    }
    platform::GraphicsEnvironment::Get().Shutdown();
    ::CoUninitialize();
    return result;
}
