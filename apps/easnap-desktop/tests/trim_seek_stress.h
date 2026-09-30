// Included by test_ui_modernization.cpp. Exercise production timeline messages
// and real native clicks while the actual isolated preview remains playing.
// No mock player, replacement HWND painter or alternate transport is used.
void TrimSeekStress(unsigned click_count = 72) {
    const auto original_theme = ui::theme::Preference();
    for (const uint32_t seconds : {6u, 10u}) {
        const auto source = std::filesystem::temp_directory_path() /
            (L"easnap_seek_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
             std::to_wstring(seconds) + L".mp4");
        const bool created = test::CreateVideoPreviewSource(source.wstring(), seconds);
        CHECK(created);
        if (!created) continue;
        for (const auto theme : {app::AppearanceTheme::Dark, app::AppearanceTheme::Light}) {
            // Each case is independently bounded, including an accidentally
            // blocked native loop; its watchdog always releases injected input.
            ResizeTestDeadline deadline;
            Appearance(theme);
            ui::TrimWindow trim({});
            CHECK(trim.Open(source.wstring(), test::OnTestMonitor({100,100,1060,640})));
            const HWND window = trim.hwnd();
            if (!window) break;
            const bool ready = WaitFor([&] {
                return ::IsWindowEnabled(::GetDlgItem(window, IDOK)) != FALSE;
            }, 15000);
            CHECK(ready);
            if (!ready) { trim.Destroy(); continue; }
            const HWND timeline = ::FindWindowExW(window,nullptr,L"EasnapTrimTimeline",nullptr);
            const HWND video = ::FindWindowExW(window,nullptr,L"EasnapTrimVideo",nullptr);
            const HWND host = ::GetWindow(video,GW_CHILD);
            DWORD helper_pid = 0;
            ::GetWindowThreadProcessId(host,&helper_pid);
            CHECK(timeline && host && helper_pid && helper_pid != ::GetCurrentProcessId());
            RECT track{}; CHECK(::GetClientRect(timeline,&track));
            const float scale = ::GetDpiForWindow(window)/96.0f;
            const int left = ui::theme::Scaled(18,scale);
            const int width = track.right-ui::theme::Scaled(36,scale);
            const int y = track.bottom/2;
            const auto coordinate = [&](unsigned percent) {
                return left+static_cast<int>(static_cast<unsigned>(width)*percent/100);
            };
            const auto target_for = [&](int x) {
                return trim.range().TimeFromTimelineOffset(static_cast<uint32_t>(x-left),
                                                          static_cast<uint32_t>(width));
            };
            const auto playing = [&] {
                wchar_t text[32]{}; ::GetWindowTextW(::GetDlgItem(window,100),text,32);
                return ::lstrcmpW(text,L"Pause") == 0;
            };
            CHECK(::SetWindowPos(window,HWND_TOPMOST,0,0,0,0,
                SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE));
            ::BringWindowToTop(window); ::SetForegroundWindow(window); Pump(100);
            if (::GetForegroundWindow()!=window) {
                const auto bounds=Bounds(window);
                const POINT activation{bounds.left+ui::theme::Scaled(230,scale),
                                       bounds.top+ui::theme::Scaled(20,scale)};
                if (::GetAncestor(::WindowFromPoint(activation),GA_ROOT)==window) {
                    CHECK(::SetCursorPos(activation.x,activation.y));
                    INPUT input[2]{};input[0].type=input[1].type=INPUT_MOUSE;
                    input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
                    CHECK(::SendInput(2,input,sizeof(INPUT))==2);Pump(100);
                }
            }
            CHECK(::GetForegroundWindow() == window);
            const auto pointer_at = [&](int x) {
                POINT expected{x,y},actual{};
                const bool matches=::ClientToScreen(timeline,&expected) && ::GetCursorPos(&actual) &&
                    actual.x==expected.x && actual.y==expected.y &&
                    ::WindowFromPoint(actual)==timeline && ::GetForegroundWindow()==window;
                if (!matches) {
                    wchar_t hit_class[96]{};
                    ::GetClassNameW(::WindowFromPoint(actual),hit_class,96);
                    std::printf("Seek input mismatch: expected=%ld,%ld actual=%ld,%ld hit=%p class=%ls timeline=%p foreground=%p window=%p\n",
                        expected.x,expected.y,actual.x,actual.y,::WindowFromPoint(actual),hit_class,
                        timeline,::GetForegroundWindow(),window);
                }
                return matches;
            };
            bool input_valid=true;
            const auto authenticate_pointer = [&](int x) {
                const bool matches=pointer_at(x);
                CHECK(matches);
                if (!matches) {
                    input_valid=false;
                    // Cancel the gesture before releasing injected input. An
                    // UP at an unrelated physical point must not issue a seek.
                    if (::GetCapture()==timeline) CHECK(::ReleaseCapture());
                    INPUT release{};release.type=INPUT_MOUSE;
                    release.mi.dwFlags=MOUSEEVENTF_LEFTUP;
                    CHECK(::SendInput(1,&release,sizeof(release))==1);
                }
                return matches;
            };
            const auto place_pointer = [&](int x) {
                POINT point{x,y}; CHECK(::ClientToScreen(timeline,&point));
                CHECK(::SetCursorPos(point.x,point.y));
                return authenticate_pointer(x);
            };
            Click(window,100); CHECK(WaitFor(playing));
            std::printf("Seek click regression: seconds=%u theme=%d dpi=%u helper=%lu\n",
                seconds,static_cast<int>(theme),::GetDpiForWindow(window),helper_pid);
            // Synchronous native messages isolate exactly which message
            // changes the real UI. The elapsed hold permits actual playback
            // advancement: comparing with the current position alone would
            // wrongly treat the release as a new seek to the old target.
            for (unsigned hold : {80u,120u,160u}) {
                const int x = coordinate(20+hold/8);
                const auto target = target_for(x);
                // Capture can synthesize a real WM_MOUSEMOVE after DOWN. Keep
                // the physical pointer and sent coordinates identical, and
                // drain the previous gesture before beginning this one.
                if (!place_pointer(x)) break;
                Pump(20); if (!authenticate_pointer(x)) break;
                ::SendMessageW(timeline,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                CHECK(::GetCapture() == timeline && trim.range().current() == target);
                Pump(hold);
                const bool move_ready=WaitFor([&] { return trim.range().current() > target+media::MediaTime{250'000}; },1500);
                if (!authenticate_pointer(x)) break;
                CHECK(move_ready);
                const auto before_move = trim.range().current();
                for (unsigned repeated = 0; repeated != 3; ++repeated) {
                    if (!authenticate_pointer(x)) break;
                    ::SendMessageW(timeline,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,y));
                }
                if (!input_valid) break;
                const auto after_move = trim.range().current();
                CHECK(after_move == before_move);
                const bool release_ready=WaitFor([&] { return trim.range().current() > target+media::MediaTime{250'000}; },1500);
                if (!authenticate_pointer(x)) break;
                CHECK(release_ready);
                const auto before_up = trim.range().current();
                ::SendMessageW(timeline,WM_LBUTTONUP,0,MAKELPARAM(x,y));
                const auto after_up = trim.range().current();
                CHECK(after_up == before_up && ::GetCapture() != timeline);
                // Suppression must end with this gesture: another click at the
                // same pixel is an intentional seek, even while still playing.
                if (!place_pointer(x)) break;
                Pump(20); if (!authenticate_pointer(x)) break;
                ::SendMessageW(timeline,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
                CHECK(trim.range().current() == target);
                const int next_x = coordinate(25);
                if (!place_pointer(next_x)) break;
                ::SendMessageW(timeline,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(next_x,y));
                CHECK(trim.range().current() == target_for(next_x));
                if (!authenticate_pointer(next_x)) break;
                ::SendMessageW(timeline,WM_LBUTTONUP,0,MAKELPARAM(next_x,y));
                CHECK(playing());
                std::printf("Stationary gesture: hold_ms=%u target=%lld move=%lld->%lld up=%lld->%lld\n",
                    hold,target.count(),before_move.count(),after_move.count(),before_up.count(),after_up.count());
            }
            if (!input_valid) {
                std::puts("Seek case aborted: pointer authentication failed; dependent seek assertions were not executed.");
                trim.Destroy();Pump(100);continue;
            }
            if (!click_count) { trim.Destroy(); Pump(100); continue; }
            CHECK(::GetForegroundWindow() == window);
            RECT client{}; CHECK(::GetClientRect(window,&client));
            ::MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&client),2);
            RECT picture = Bounds(video); ::OffsetRect(&picture,-client.left,-client.top);
            test::PresentedDesktopCapture capture(client);
            const HWND chrome=::FindWindowExW(window,nullptr,L"EasnapWindowChrome",nullptr);
            const auto expected_caption=RenderControl(chrome);
            const auto caption_bounds=Bounds(chrome);
            const auto stem = L"seek-"+std::to_wstring(seconds)+L"-theme-"+
                std::to_wstring(static_cast<int>(theme));
            std::shared_ptr<const platform::BgraImage> preparation_frame;
            int mismatch_x=0,mismatch_y=0;
            uint32_t mismatch_actual=0,mismatch_expected=0;
            // Authenticate the asynchronously delivered desktop before any
            // stress input. Current HWND ownership alone cannot distinguish an
            // older captured desktop frame from the currently visible window.
            const bool capture_ready=WaitFor([&] {
                auto frame=capture.CaptureNext();
                if (!frame) return false;
                preparation_frame=std::move(frame);
                for (int row=ui::theme::Scaled(10,scale);row<ui::theme::Scaled(30,scale);++row)
                    for (int col=ui::theme::Scaled(20,scale);col<ui::theme::Scaled(210,scale);++col) {
                        const int cx=client.left+col-caption_bounds.left;
                        const int cy=client.top+row-caption_bounds.top;
                        if (cx<0 || cy<0 || cx>=expected_caption.width || cy>=expected_caption.height) return false;
                        if (preparation_frame->row(row)[col]!=expected_caption.row(cy)[cx]) {
                            mismatch_x=col;mismatch_y=row;
                            mismatch_actual=preparation_frame->row(row)[col];
                            mismatch_expected=expected_caption.row(cy)[cx];
                            return false;
                        }
                    }
                return true;
            },2000);
            CHECK(capture_ready);
            if (!capture_ready) {
                std::printf("Seek caption mismatch: x=%d y=%d actual=%08x expected=%08x client=%ld,%ld caption=%ld,%ld\n",
                    mismatch_x,mismatch_y,mismatch_actual,mismatch_expected,client.left,client.top,caption_bounds.left,caption_bounds.top);
                if (!evidence.empty()) {
                    if (preparation_frame) CHECK(platform::SaveImage(*preparation_frame,(evidence/(stem+L"-unprepared.png")).wstring()));
                    CHECK(platform::SaveImage(expected_caption,(evidence/(stem+L"-expected-caption.png")).wstring()));
                }
                trim.Destroy();Pump(100);continue;
            }
            std::ofstream samples;
            if (!evidence.empty()) {
                samples.open(evidence/(stem+L".csv"));
                samples << "elapsed_ms,click,position,helper_pid,enabled,playing,frame,blank\n";
            }
            const auto begin = ::GetTickCount64();
            unsigned active_click = 0, observations = 0, disabled = 0, lost_play = 0;
            unsigned replaced = 0, frames = 0, blank = 0, moving = 0, completed = 0;
            uint64_t previous_hash = 0;
            ULONGLONG last_change = begin, max_frozen = 0, max_pump = 0;
            const auto observe = [&] {
                const auto now = ::GetTickCount64();
                ++observations;
                bool enabled = ::IsWindowEnabled(timeline) != FALSE;
                for (const int id : {100,101,102,IDOK,104,IDCANCEL})
                    enabled = enabled && ::IsWindowEnabled(::GetDlgItem(window,id));
                disabled += !enabled;
                const bool play = playing(); lost_play += !play;
                const HWND current_host = ::GetWindow(video,GW_CHILD);
                DWORD current_pid = 0; ::GetWindowThreadProcessId(current_host,&current_pid);
                replaced += current_host != host || current_pid != helper_pid;
                const auto frame = capture.CaptureNext();
                bool absent = false;
                if (frame) {
                    ++frames;
                    unsigned red_min=255,green_min=255,blue_min=255;
                    unsigned red_max=0,green_max=0,blue_max=0;
                    uint64_t hash = 14695981039346656037ull;
                    for (int row=picture.top+8;row<picture.bottom-8;row+=17) {
                        for (int col=picture.left+8;col<picture.right-8;col+=17) {
                            const auto pixel=frame->row(row)[col];
                            const auto red=(pixel>>16)&255u,green=(pixel>>8)&255u,blue=pixel&255u;
                            red_min=(std::min)(red_min,red);red_max=(std::max)(red_max,red);
                            green_min=(std::min)(green_min,green);green_max=(std::max)(green_max,green);
                            blue_min=(std::min)(blue_min,blue);blue_max=(std::max)(blue_max,blue);
                            hash=(hash^pixel)*1099511628211ull;
                        }
                    }
                    // The real encoded gradient spans a broad color range in
                    // every frame. A uniform black/panel replacement cannot.
                    absent = red_max-red_min<12 && green_max-green_min<12 && blue_max-blue_min<12;
                    blank += absent;
                    if (previous_hash && hash != previous_hash) { ++moving; last_change=now; }
                    previous_hash=hash;
                    if (!evidence.empty() && ((absent && blank<=3) || frames==1))
                        CHECK(platform::SaveImage(*frame,(evidence/(stem+L"-frame-"+std::to_wstring(frames)+L".png")).wstring()));
                }
                max_frozen=(std::max)(max_frozen,now-last_change);
                if (samples) samples << now-begin << ',' << active_click << ',' << trim.range().current().count()
                    << ',' << current_pid << ',' << enabled << ',' << play << ',' << !!frame << ',' << absent << '\n';
            };
            const auto pump_observed = [&](unsigned milliseconds) {
                const auto until=::GetTickCount64()+milliseconds;
                do {
                    const auto before=::GetTickCount64(); Pump(1);
                    max_pump=(std::max)(max_pump,::GetTickCount64()-before);
                    observe();
                } while (::GetTickCount64()<until);
            };
            const auto mouse = [](DWORD flags) {
                INPUT input{}; input.type=INPUT_MOUSE; input.mi.dwFlags=flags;
                return ::SendInput(1,&input,sizeof(input)) == 1;
            };
            uint32_t seed=0x5ea7c11cu^seconds^static_cast<uint32_t>(theme);
            const auto random = [&] { seed=seed*1664525u+1013904223u; return seed; };
            for (active_click=0;active_click<click_count;++active_click) {
                // Leave at least two seconds before EOS. The test never
                // pauses/restarts playback between clicks to hide an outage.
                const int x=coordinate(15+random()%46);
                POINT point{x,y}; CHECK(::ClientToScreen(timeline,&point));
                CHECK(::SetCursorPos(point.x,point.y)); Pump(1);
                const auto pressed_at=::GetTickCount64();
                const unsigned interval=100+random()%801;
                const unsigned hold=(std::min)(80+random()%81,interval-20);
                if (!authenticate_pointer(x)) break;
                CHECK(mouse(MOUSEEVENTF_LEFTDOWN));
                pump_observed(20);
                // SendInput delivery is asynchronous. Observe while waiting
                // for actual capture instead of assuming a 20 ms dispatch.
                while (::GetCapture()!=timeline && ::GetTickCount64()<pressed_at+250)
                    pump_observed(1);
                const bool captured=::GetCapture()==timeline;
                CHECK(captured);
                if (!captured) { CHECK(mouse(MOUSEEVENTF_LEFTUP)); break; }
                const auto held=::GetTickCount64()-pressed_at;
                const unsigned remaining=held<hold ? hold-static_cast<unsigned>(held) : 0;
                if (remaining/2) pump_observed(remaining/2);
                // Duplicate same-pixel moves commonly arrive during a held
                // click; deliberately deliver them through the real input queue.
                if (!authenticate_pointer(x)) break;
                CHECK(mouse(MOUSEEVENTF_MOVE));
                if (remaining-remaining/2) pump_observed(remaining-remaining/2);
                if (!authenticate_pointer(x)) break;
                CHECK(mouse(MOUSEEVENTF_LEFTUP));
                const auto released_at=::GetTickCount64();
                if (released_at<pressed_at+interval)
                    pump_observed(static_cast<unsigned>(pressed_at+interval-released_at));
                else pump_observed(1);
                CHECK(::GetCapture()!=timeline);
                ++completed;
            }
            if (!input_valid) {
                std::printf("Seek case aborted after %u/%u native clicks: pointer authentication failed.\n",completed,click_count);
                trim.Destroy();Pump(100);continue;
            }
            CHECK(mouse(MOUSEEVENTF_LEFTUP));
            pump_observed(500);
            std::printf("Seek stress: seconds=%u theme=%d clicks=%u/%u elapsed_ms=%llu observations=%u frames=%u moving=%u blank=%u disabled=%u lost_play=%u helper_changed=%u max_frozen_ms=%llu max_pump_ms=%llu\n",
                seconds,static_cast<int>(theme),completed,click_count,::GetTickCount64()-begin,
                observations,frames,moving,blank,disabled,lost_play,replaced,max_frozen,max_pump);
            CHECK(completed==click_count && observations>click_count && frames>=30 && moving>=20);
            CHECK(blank==0 && disabled==0 && lost_play==0 && replaced==0);
            CHECK(max_frozen<1000 && max_pump<1000);
            trim.Destroy(); Pump(100); CHECK(!::IsWindow(host));
        }
        std::error_code error; std::filesystem::remove(source,error); CHECK(!error);
    }
    Appearance(original_theme);
}
