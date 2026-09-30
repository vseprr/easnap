// Included by test_ui_modernization.cpp. Observe the real production trim and
// helper on the monitor, including the UI surrounding the video at EOS.
void TrimEndpointPresentation(const std::filesystem::path& supplied_source = {}) {
    const bool generated = supplied_source.empty();
    const auto source = generated ? std::filesystem::temp_directory_path() /
        (L"easnap_trim_endpoint_" + std::to_wstring(::GetCurrentProcessId()) + L".mp4") : std::filesystem::absolute(supplied_source);
    if (generated) CHECK(test::CreateVideoPreviewSource(source.wstring()));
    const auto original_theme = ui::theme::Preference();
    for (auto theme : {app::AppearanceTheme::Dark, app::AppearanceTheme::Light}) {
        Appearance(theme);
        ui::TrimWindow trim({});
        CHECK(trim.Open(source.wstring(), test::OnTestMonitor({0, 0, 1920, 1080})));
        const HWND window = trim.hwnd();
        if (!window) break;
        CHECK(WaitFor([&] { return ::IsWindowEnabled(::GetDlgItem(window, IDOK)); }, 15000));
        const HWND timeline = ::FindWindowExW(window, nullptr, L"EasnapTrimTimeline", nullptr);
        // Populate the paused compositor backing at a different size, then
        // maximize/restore as in the supplied recording before normal playback.
        ::SendMessageW(timeline, WM_KEYDOWN, VK_RIGHT, 0);
        Pump(200);
        ::ShowWindow(window, SW_MAXIMIZE); Pump(200);
        ::ShowWindow(window, SW_RESTORE); Pump(200);
        ::ShowWindow(window, SW_MAXIMIZE); Pump(300);
        ::SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RECT client{}; ::GetClientRect(window, &client);
        ::MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
        const HWND video = ::FindWindowExW(window, nullptr, L"EasnapTrimVideo", nullptr);
        RECT picture = Bounds(video);
        ::OffsetRect(&picture, -client.left, -client.top);
        CHECK(picture.right > picture.left && picture.bottom > picture.top);
        std::printf("Trim endpoint: theme=%d dpi=%u client=%ldx%ld video=%ld,%ld,%ld,%ld\n",
            static_cast<int>(theme), ::GetDpiForWindow(window), client.right-client.left,
            client.bottom-client.top, picture.left, picture.top, picture.right, picture.bottom);
        test::PresentedDesktopCapture capture(client);
        ::SendMessageW(timeline, WM_KEYDOWN, VK_HOME, 0); Pump(200);
        for (unsigned cycle = 0; cycle != 2; ++cycle) {
            Click(window, 100); // The second cycle replays from the retained Out.
            const auto deadline = ::GetTickCount64() + trim.range().source_duration().count()/10000 + 5000;
            ULONGLONG ended_at = 0;
            bool started = false;
            unsigned frames = 0, near_end = 0, damaged = 0;
            unsigned moving_frames = 0;
            uint64_t previous_picture = 0;
            bool saved_playback = false, saved_settled = false;
            const auto color_matches = [](uint32_t pixel, COLORREF color) {
                return std::abs(static_cast<int>((pixel >> 16) & 255) - GetRValue(color)) <= 3 &&
                    std::abs(static_cast<int>((pixel >> 8) & 255) - GetGValue(color)) <= 3 &&
                    std::abs(static_cast<int>(pixel & 255) - GetBValue(color)) <= 3;
            };
            while (::GetTickCount64() < deadline && (!ended_at || ::GetTickCount64()-ended_at < 300)) {
                Pump(1);
                started |= trim.range().current() < trim.range().out();
                if (started && trim.range().current() == trim.range().out() && !ended_at) ended_at = ::GetTickCount64();
                const auto frame = capture.CaptureNext();
                if (!frame) continue;
                ++frames;
                if (trim.range().current() > media::MediaTime{5'000'000} &&
                    trim.range().current() < trim.range().out()-media::MediaTime{5'000'000}) {
                    uint64_t picture_hash = 14695981039346656037ull;
                    for (int y = picture.top+8; y < picture.bottom-8; y += 29)
                        for (int x = picture.left+8; x < picture.right-8; x += 29)
                            picture_hash = (picture_hash ^ frame->row(y)[x]) * 1099511628211ull;
                    if (previous_picture && picture_hash != previous_picture) ++moving_frames;
                    previous_picture = picture_hash;
                    if (!saved_playback && !evidence.empty()) {
                        CHECK(platform::SaveImage(*frame, (evidence/(L"endpoint-"+
                            std::to_wstring(static_cast<int>(theme))+L"-cycle-"+std::to_wstring(cycle)+L"-playing.png")).wstring()));
                        saved_playback = true;
                    }
                }
                if (trim.range().current() >= trim.range().out()-media::MediaTime{5'000'000}) ++near_end;
                // A duplicated timeline is a long coral strip inside the picture;
                // a stale parent backing also inserts a long panel-colored strip.
                // Neither exists in the generated moving gradient or supplied clip.
                bool corrupt = false;
                unsigned dark = 0, samples = 0;
                for (int y = picture.top+3; y < picture.bottom-3; y += 3) {
                    int coral_run = 0, panel_run = 0;
                    for (int x = picture.left+3; x < picture.right-3; ++x) {
                        const auto pixel = frame->row(y)[x];
                        coral_run = color_matches(pixel, ui::theme::Current().timeline_selection) ? coral_run+1 : 0;
                        panel_run = color_matches(pixel, ui::theme::Current().panel) ? panel_run+1 : 0;
                        corrupt |= coral_run > (picture.right-picture.left)/4 || panel_run > (picture.right-picture.left)/2;
                        dark += (pixel & 0xffffff) == 0;
                        ++samples;
                    }
                }
                corrupt |= samples && dark > samples*98/100;
                if (corrupt) {
                    std::printf("Damaged frame observed at %llu position=%lld\n", ::GetTickCount64(), trim.range().current().count());
                    ++damaged;
                    if (!evidence.empty() && damaged <= 3) CHECK(platform::SaveImage(*frame,
                        (evidence/(L"endpoint-"+std::to_wstring(static_cast<int>(theme))+L"-cycle-"+std::to_wstring(cycle)+L"-bad-"+std::to_wstring(damaged)+L".png")).wstring()));
                }
                // Desktop capture can trail the logical Out notification.
                // Save the held result after composition has settled, rather
                // than mislabeling a queued frame from before the handoff.
                if (ended_at && !evidence.empty() && !saved_settled && ::GetTickCount64()-ended_at >= 200) {
                    CHECK(platform::SaveImage(*frame, (evidence/(L"endpoint-"+std::to_wstring(static_cast<int>(theme))+L"-settled.png")).wstring()));
                    saved_settled = true;
                }
            }
            std::printf("Trim endpoint: cycle=%u frames=%u moving=%u near_end=%u damaged=%u completed=%d\n", cycle, frames, moving_frames, near_end, damaged, ended_at != 0);
            CHECK(frames >= 30 && moving_frames >= 10 && near_end >= 3 && ended_at && damaged == 0);
        }
        trim.Destroy(); Pump(100);
    }
    Appearance(original_theme);
    if (generated) { std::error_code error; std::filesystem::remove(source, error); CHECK(!error); }
}
