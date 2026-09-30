// Included by test_ui_modernization.cpp inside its test namespace. Observe
// actual desktop pixels; never request a repaint during observed motion.
class ResizeTestDeadline {
public:
    ResizeTestDeadline() : worker_([this] {
        std::unique_lock lock(mutex_);
        if(!completed_.wait_for(lock,std::chrono::seconds(120),[this] { return done_; })) {
            // A blocked native sizing loop must not leave injected input down.
            INPUT mouse{};mouse.type=INPUT_MOUSE;mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP;
            ::SendInput(1,&mouse,sizeof(mouse));
            std::fputs("Native resize test exceeded its deadline.\n",stderr);
            ::TerminateProcess(::GetCurrentProcess(),2);
        }
    }) {}
    ~ResizeTestDeadline() {
        { const std::lock_guard lock(mutex_);done_=true; }
        completed_.notify_one();worker_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable completed_;
    bool done_{};
    std::thread worker_;
};

void AllEdgeResizePresentation(ui::TrimWindow& trim, const wchar_t* mode, const wchar_t* edge_filter=nullptr, bool external_capture=false) {
    ResizeTestDeadline deadline;
    wchar_t speed[8]{};
    const bool slow=::GetEnvironmentVariableW(L"EASNAP_TEST_RESIZE_SLOW",speed,8)==1 && speed[0]==L'1';
    const bool playing=::lstrcmpW(mode,L"playing")==0;
    const int period=slow ? 3200:400,duration=slow ? 3200:1600;
    std::printf("Resize matrix state=%ls speed=%s period_ms=%d\n",mode,slow ? "slow":"fast",period);
    const HWND window=trim.hwnd();
    BOOL composition=FALSE;const HRESULT composition_result=::DwmIsCompositionEnabled(&composition);
    const BOOL no_transitions=TRUE;
    const HRESULT transition_result=::DwmSetWindowAttribute(window,DWMWA_TRANSITIONS_FORCEDISABLED,&no_transitions,sizeof(no_transitions));
    std::printf("Resize desktop: composition=%d hr=%08lx transitions=%08lx dpi=%u thread_awareness=%d\n",composition,
        static_cast<unsigned long>(composition_result),static_cast<unsigned long>(transition_result),::GetDpiForWindow(window),
        static_cast<int>(::GetAwarenessFromDpiAwarenessContext(::GetThreadDpiAwarenessContext())));
    const RECT original=Bounds(window);
    const auto old_theme=ui::theme::Preference();
    const auto work=platform::MonitorForRect(original).work_area;
    const float scale=::GetDpiForWindow(window)/96.0f;
    const auto dip=[&](int value) { return ui::theme::Scaled(static_cast<float>(value),scale); };
    const int width=std::min(dip(1000),static_cast<int>(work.right-work.left)-80);
    const int height=std::min(dip(700),static_cast<int>(work.bottom-work.top)-80);
    MINMAXINFO limits{};::SendMessageW(window,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&limits));
    const int min_width=limits.ptMinTrackSize.x,min_height=limits.ptMinTrackSize.y;
    CHECK(width>min_width && height>min_height);
    HWND range=nullptr;
    for(HWND child=::GetWindow(window,GW_CHILD);child;child=::GetWindow(child,GW_HWNDNEXT)) {
        wchar_t title[256]{}; ::GetWindowTextW(child,title,256);
        if(std::wstring_view(title).starts_with(L"In ")) { range=child; break; }
    }
    const HWND set_in=::GetDlgItem(window,101);
    const HWND chrome=::FindWindowExW(window,nullptr,L"EasnapWindowChrome",nullptr);
    const HWND maximize=::FindWindowExW(chrome,nullptr,L"BUTTON",L"Maximize or restore");
    CHECK(range && set_in && maximize);
    if(!range || !set_in || !maximize) return;
    constexpr uint32_t green=0xff0df171;
    HBRUSH brush=::CreateSolidBrush(RGB(13,241,113));
    WNDCLASSW cls{}; cls.lpfnWndProc=::DefWindowProcW; cls.hInstance=::GetModuleHandleW(nullptr);
    cls.hbrBackground=brush; cls.lpszClassName=L"EasnapAllEdgeBackdrop";
    CHECK(::RegisterClassW(&cls));
    HWND backdrop=::CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Resize background",WS_POPUP|WS_VISIBLE,
        work.left,work.top,work.right-work.left,work.bottom-work.top,nullptr,nullptr,cls.hInstance,nullptr);
    struct Geometry { RECT client; std::array<RECT,16> patches; };
    struct Probe {
        HWND owner{},range{},set_in{}; float scale{};
        std::array<HWND,8> extra_controls{};
        HWND time{},timeline{},status{},progress{};
        ui::TrimWindow* trim{};
        int line_height{};
        bool cancel_visible{};
        unsigned paints{},sizes{};
        long long paint_us{},size_us{},paint_max_us{},size_max_us{};
        std::mutex mutex;
        std::vector<Geometry> history;
        std::atomic<bool> sizing{false};
        std::atomic<unsigned> moves{0};
        ULONGLONG resize_started{};
        unsigned measured_moves{};
        ULONGLONG resize_total{},resize_max{};
        void Record(HWND hwnd) {
            Geometry next{}; ::GetClientRect(hwnd,&next.client);
            ::MapWindowPoints(hwnd,nullptr,reinterpret_cast<POINT*>(&next.client),2);
            const auto d=[&](int v) {return ui::theme::Scaled(static_cast<float>(v),scale);};
            next.patches[0]={next.client.left+d(20),next.client.top+d(10),next.client.left+d(210),next.client.top+d(30)};
            const RECT label=Bounds(range),button=Bounds(set_in);
            const LONG cy=(label.top+label.bottom)/2,by=(button.top+button.bottom)/2,bx=(button.left+button.right)/2;
            next.patches[1]={label.left,cy-d(9),label.left+d(160),cy+d(9)};
            next.patches[2]={bx-d(30),by-d(9),bx+d(30),by+d(9)};
            for(size_t i=0;i<extra_controls.size();++i) {
                const RECT box=Bounds(extra_controls[i]);
                const LONG x=(box.left+box.right)/2,y=(box.top+box.bottom)/2;
                const int half_width=d(i<5 ? 30:8),half_height=d(i<5 ? 9:8);
                // Icon+label buttons draw text at a fixed left inset, whereas
                // Set Out and caption glyphs are centered. Follow each actual
                // painter's anchor at every width; the center of a wide icon
                // button is blank but contains its label at minimum width.
                next.patches[3+i]=i>=1 && i<=4 && !(i==2 && cancel_visible) ?
                    RECT{box.left+d(40),y-half_height,box.left+d(100),y+half_height}:
                    RECT{x-half_width,y-half_height,x+half_width,y+half_height};
            }
            const RECT clock=Bounds(time);const LONG clock_y=(clock.top+clock.bottom)/2;
            // The right-aligned duration is constant while the current time
            // advances. The moving current-time field is verified separately.
            next.patches[11]={clock.right-d(70),clock_y-d(9),clock.right,clock_y+d(9)};
            const RECT timeline_box=Bounds(timeline);
            const int track_width=std::max(1,static_cast<int>(timeline_box.right-timeline_box.left)-d(36));
            for(size_t i=0;i<2;++i) {
                // Match PaintTimeline's label anchors, using the production
                // range model for the time-to-track mapping.
                const auto value=i==0 ? trim->range().in():trim->range().out();
                const LONG x=timeline_box.left+d(18)+static_cast<LONG>(trim->range().TimelineOffsetFromTime(value,static_cast<uint32_t>(track_width)));
                next.patches[12+i]={x-d(16),timeline_box.top,x+d(16),timeline_box.top+line_height};
            }
            if(::IsWindowVisible(status)) {
                RECT text{};::SendMessageW(status,EM_GETRECT,0,reinterpret_cast<LPARAM>(&text));
                ::MapWindowPoints(status,nullptr,reinterpret_cast<POINT*>(&text),2);
                next.patches[14]={text.left,text.top,text.left+d(100),text.top+line_height};
            }
            if(::IsWindowVisible(progress)) {
                const RECT bar=Bounds(progress);const LONG y=(bar.top+bar.bottom)/2;
                // Stable filled interior at 42%; verify its actual presented
                // location without treating a width change as pixel corruption.
                next.patches[15]={bar.left+d(10),y-d(2),bar.left+d(25),y+d(2)};
            }
            const std::lock_guard lock(mutex);
            if(history.size()==32) history.erase(history.begin());
            history.push_back(next);
        }
        static LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR ref) {
            auto& self=*reinterpret_cast<Probe*>(ref);
            const auto started=std::chrono::steady_clock::now();
            if(hwnd!=self.owner) {
                const auto result=::DefSubclassProc(hwnd,message,wparam,lparam);
                if(message==WM_PAINT) {
                    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
                    ++self.paints;self.paint_us+=us;self.paint_max_us=std::max(self.paint_max_us,us);
                }
                return result;
            }
            if(message==WM_WINDOWPOSCHANGING && !(reinterpret_cast<WINDOWPOS*>(lparam)->flags&SWP_NOSIZE)) self.resize_started=::GetTickCount64();
            if(message==WM_ENTERSIZEMOVE) self.sizing=true;
            if(message==WM_EXITSIZEMOVE) self.sizing=false;
            const auto result=::DefSubclassProc(hwnd,message,wparam,lparam);
            if(message==WM_SIZE) {
                const auto us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
                ++self.sizes;self.size_us+=us;self.size_max_us=std::max(self.size_max_us,us);
            }
            if(message==WM_WINDOWPOSCHANGED && !(reinterpret_cast<WINDOWPOS*>(lparam)->flags&SWP_NOSIZE) && self.resize_started) {
                const auto elapsed=::GetTickCount64()-self.resize_started;
                self.resize_total+=elapsed;self.resize_max=std::max(self.resize_max,elapsed);++self.measured_moves;self.resize_started=0;
            }
            if(message==WM_SIZE || message==WM_WINDOWPOSCHANGED) {self.Record(hwnd);++self.moves;}
            return result;
        }
    } probe;
    probe.owner=window;probe.range=range;probe.set_in=set_in;probe.scale=scale;probe.trim=&trim;
    probe.timeline=::FindWindowExW(window,nullptr,L"EasnapTrimTimeline",nullptr);
    probe.status=::FindWindowExW(window,nullptr,L"EDIT",nullptr);
    probe.progress=::FindWindowExW(window,nullptr,PROGRESS_CLASSW,nullptr);
    probe.cancel_visible=::IsWindowVisible(::GetDlgItem(window,103))!=FALSE;
    HDC measure=::CreateCompatibleDC(nullptr);
    const HFONT font=reinterpret_cast<HFONT>(::SendMessageW(range,WM_GETFONT,0,0));
    CHECK(measure && font);
    const auto old_font=::SelectObject(measure,font);
    TEXTMETRICW metrics{};CHECK(::GetTextMetricsW(measure,&metrics));probe.line_height=metrics.tmHeight;
    ::SelectObject(measure,old_font);::DeleteDC(measure);
    probe.extra_controls={::GetDlgItem(window,102),::GetDlgItem(window,100),::GetDlgItem(window,probe.cancel_visible ? 103:IDOK),
        ::GetDlgItem(window,104),::GetDlgItem(window,IDCANCEL),
        ::FindWindowExW(chrome,nullptr,L"BUTTON",L"Close"),
        ::FindWindowExW(chrome,nullptr,L"BUTTON",L"Minimize"),maximize};
    for(HWND child=::GetWindow(window,GW_CHILD);child;child=::GetWindow(child,GW_HWNDNEXT)) {
        wchar_t title[256]{};::GetWindowTextW(child,title,256);
        if(std::wstring_view(title).find(L" / ")!=std::wstring_view::npos) { probe.time=child;break; }
    }
    CHECK(probe.time && std::all_of(probe.extra_controls.begin(),probe.extra_controls.end(),[](HWND control){return control!=nullptr;}));
    if(!probe.time) { ::DestroyWindow(backdrop);::UnregisterClassW(cls.lpszClassName,cls.hInstance);::DeleteObject(brush);return; }
    CHECK(::SetWindowSubclass(window,Probe::Proc,0xEA70,reinterpret_cast<DWORD_PTR>(&probe)));
    ::EnumChildWindows(window,[](HWND child,LPARAM ref)->BOOL {
        if(::GetWindowThreadProcessId(child,nullptr)==::GetCurrentThreadId())
            CHECK(::SetWindowSubclass(child,Probe::Proc,0xEA70,static_cast<DWORD_PTR>(ref)));
        return TRUE;
    },reinterpret_cast<LPARAM>(&probe));
    struct Edge { int hit; const wchar_t* name; int x,y; };
    const std::array<Edge,10> edges{{{HTRIGHT,L"right",1,0},{HTBOTTOMRIGHT,L"bottom-right",1,1},
        {HTBOTTOM,L"bottom",0,1},{HTBOTTOMLEFT,L"bottom-left",-1,1},{HTLEFT,L"left",-1,0},
        {HTTOPLEFT,L"top-left",-1,-1},{HTTOP,L"top",0,-1},{HTTOPRIGHT,L"top-right",1,-1},{HTCAPTION,L"caption-move",0,0},{0,L"maximize",0,0}}};
    POINT old_cursor{};const bool restore_cursor=::GetCursorPos(&old_cursor)!=FALSE;
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) for(const auto& edge:edges) {
        if(edge_filter && ::lstrcmpW(edge_filter,edge.name)!=0) continue;
        Appearance(theme);
        ::ShowWindow(window,SW_RESTORE);
        CHECK(::SetWindowPos(backdrop,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE));
        CHECK(::SetWindowPos(window,HWND_TOPMOST,work.left+40,work.top+40,width,height,SWP_SHOWWINDOW));
        ::BringWindowToTop(window);::SetForegroundWindow(window);
        if(playing) {
            const HWND timeline=::FindWindowExW(window,nullptr,L"EasnapTrimTimeline",nullptr);
            CHECK(AutomationClient([&]{return ProbeTimeline(timeline,true);}));
            CHECK(WaitFor([&]{return trim.range().current()==trim.range().out();},3000));
            ::SendMessageW(::GetDlgItem(window,100),BM_CLICK,0,0);
            CHECK(WaitFor([&]{return trim.range().current()>trim.range().in() && trim.range().current()<trim.range().out();},3000));
        }
        const RECT initial=Bounds(window);
        if(::GetForegroundWindow()!=window) {
            // A background test launcher may not have foreground permission.
            // Activate through real input, as the user does, before asserting
            // ownership. Never click a point covered by an unrelated window.
            const POINT activation{initial.left+dip(200),initial.top+dip(20)};
            if(::GetAncestor(::WindowFromPoint(activation),GA_ROOT)==window) {
                ::SetCursorPos(activation.x,activation.y);
                INPUT click[2]{};click[0].type=click[1].type=INPUT_MOUSE;
                click[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN;click[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
                CHECK(::SendInput(2,click,sizeof(INPUT))==2);Pump(100);
            }
        }
        const POINT start=edge.hit==HTCAPTION?POINT{initial.left+dip(200),initial.top+dip(20)}:POINT{edge.x<0 ? initial.left+2:edge.x>0 ? initial.right-2:(initial.left+initial.right)/2,
            edge.y<0 ? initial.top+2:edge.y>0 ? initial.bottom-2:(initial.top+initial.bottom)/2};
        ::SetCursorPos(edge.hit?start.x:initial.left+dip(10),edge.hit?start.y:initial.top+dip(20));
        Pump(250);
        CHECK(::GetForegroundWindow()==window && ::GetAncestor(::WindowFromPoint(start),GA_ROOT)==window);
        {const std::lock_guard lock(probe.mutex);probe.history.clear();} probe.Record(window);
        // A newly started monitor capture can deliver a desktop frame from
        // before this HWND became visible. WindowFromPoint reports current
        // ownership, not ownership at capture time. Authenticate the initial
        // desktop title against the actual owned caption painter before using
        // any captured pixels as the reference. Never print the foreign video.
        const auto expected_caption=external_capture ? platform::BgraImage{} : RenderControl(chrome);
        const RECT caption_bounds=Bounds(chrome);
        if(edge.hit==HTCAPTION) {
            // Shared chrome is a client child: it forwards a real client
            // mouse-down to the owner's native caption move loop.
            CHECK(::WindowFromPoint(start)==chrome);
            CHECK(::SendMessageW(chrome,WM_NCHITTEST,0,MAKELPARAM(start.x,start.y))==HTCLIENT);
        } else {
            CHECK(!edge.hit || ::SendMessageW(window,WM_NCHITTEST,0,MAKELPARAM(start.x,start.y))==edge.hit);
        }
        std::atomic<bool> stop{false},ready{false},observe{false},done{false},settled{false};
        unsigned frames=0,bad=0,legacy_bad=0,holes=0,errors=0,settled_frames=0,settled_bad=0;bool input_ok=true;
        RECT reached_minimum{};
        struct Picture {
            std::wstring name;
            std::shared_ptr<const platform::BgraImage> image;
            Geometry reference;
            std::vector<Geometry> history;
            RECT common{};
        };
        std::vector<Picture> pictures;
        std::thread observer([&] {
            if(external_capture) {
                // Diagnostic isolation: keep identical backdrop, input,
                // geometry probes and DWM attributes, but no in-process
                // capture/comparison. An external recorder must inspect it.
                ready=true;
                while(!stop) ::Sleep(2);
                return;
            }
            const HRESULT initialized=::CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            try {
            winrt::check_hresult(initialized);
            test::PresentedDesktopCapture provider(work);
            std::shared_ptr<const platform::BgraImage> reference;
            Geometry reference_geometry{};
            while(!stop) {
                std::vector<Geometry> history;
                {const std::lock_guard lock(probe.mutex);history=probe.history;}
                if(history.empty()) {::Sleep(2);continue;}
                const auto captured=provider.CaptureNext();
                if(!captured) { ::Sleep(2);continue; }
                {const std::lock_guard lock(probe.mutex);history.insert(history.end(),probe.history.begin(),probe.history.end());}
                if(!reference) {
                    const RECT title=history.back().patches[0];
                    if(::GetAncestor(::WindowFromPoint({title.left,title.top}),GA_ROOT)!=window) continue;
                    bool title_ready=true;
                    for(int y=title.top;y<title.bottom && title_ready;++y) for(int x=title.left;x<title.right;++x) {
                        const int cx=x-caption_bounds.left,cy=y-caption_bounds.top;
                        const int sx=x-work.left,sy=y-work.top;
                        if(cx<0 || cy<0 || cx>=expected_caption.width || cy>=expected_caption.height ||
                            sx<0 || sy<0 || sx>=captured->width || sy>=captured->height ||
                            captured->row(sy)[sx]!=expected_caption.row(cy)[cx]) {
                            title_ready=false;break;
                        }
                    }
                    if(!title_ready) continue;
                    reference=captured;reference_geometry=history.back();
                    if(!evidence.empty()) pictures.push_back({L"reference",captured,reference_geometry,history,{}});
                    ready=true;continue;
                }
                if(!observe) continue;
                ++frames;
                const auto matches=[&](const RECT& actual,const RECT& expected,int dx,int dy) {
                    for(int y=0;y<expected.bottom-expected.top;++y) for(int x=0;x<expected.right-expected.left;++x) {
                        const int ax=actual.left-work.left+x+dx,ay=actual.top-work.top+y+dy;
                        if(ax<0 || ay<0 || ax>=captured->width || ay>=captured->height ||
                            captured->row(ay)[ax]!=reference->row(expected.top-work.top+y)[expected.left-work.left+x]) return false;
                    }
                    return true;
                };
                bool coherent=false,legacy_coherent=false;
                for(const auto& geometry:history) {
                    bool match=true;
                    for(size_t i=0;i<geometry.patches.size() && match;++i) {
                        // Centered text can shift one pixel when a control's
                        // integer width/height changes parity at fractional DPI.
                        bool part=false;
                        for(int dy=-1;dy<=1 && !part;++dy) for(int dx=-1;dx<=1 && !part;++dx)
                            part=matches(geometry.patches[i],reference_geometry.patches[i],dx,dy);
                        match=part;
                        if(i==2 && match) legacy_coherent=true;
                    }
                    if(match) {coherent=true;break;}
                }
                RECT common=history.front().client;
                for(const auto& geometry:history) ::IntersectRect(&common,&common,&geometry.client);
                ::InflateRect(&common,-dip(14),-dip(14));
                size_t exposed=0;
                for(int y=common.top;y<common.bottom;y+=3) for(int x=common.left;x<common.right;x+=3)
                    if(captured->row(y-work.top)[x-work.left]==green) ++exposed;
                if(!coherent) ++bad;
                if(!legacy_coherent) ++legacy_bad;
                if(exposed>=16) ++holes;
                if(settled) {
                    ++settled_frames;
                    if(!coherent || exposed>=16) ++settled_bad;
                }
                // Keep each failure class, including failures after input ends.
                // One early mixed frame must not hide later exposed background.
                if(!evidence.empty() && ((!coherent && bad<=2) || (exposed>=16 && holes<=2) || settled_frames==1 ||
                   (settled && settled_bad==1 && (!coherent || exposed>=16)))) {
                    pictures.push_back({L"frame-"+std::to_wstring(frames)+L"-mixed-"+std::to_wstring(!coherent)+
                        L"-holes-"+std::to_wstring(exposed)+L"-settled-"+std::to_wstring(settled.load()),
                        captured,reference_geometry,history,common});
                }
            }
            } catch(const winrt::hresult_error& error) {
                ++errors;std::printf("Desktop observer failed: 0x%08lx\n",static_cast<unsigned long>(error.code().value));
            } catch(const std::exception& error) {
                ++errors;std::printf("Desktop observer failed: %s\n",error.what());
            }
            if(SUCCEEDED(initialized)) ::CoUninitialize();
        });
        const bool prepared=WaitFor([&]{return ready.load();},3000);CHECK(prepared);
        if(prepared) {
            probe.measured_moves=0;probe.resize_total=0;probe.resize_max=0;
            probe.paints=probe.sizes=0;probe.paint_us=probe.size_us=probe.paint_max_us=probe.size_max_us=0;
            ::SetWindowPos(window,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_SHOWWINDOW);
            ::SetForegroundWindow(window);Pump(100);
            CHECK(::GetAncestor(::WindowFromPoint(start),GA_ROOT)==window);
            observe=true;
            if(edge.hit) {
                std::thread input([&] {
                    INPUT mouse{};mouse.type=INPUT_MOUSE;mouse.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
                    input_ok=::SetCursorPos(start.x,start.y)!=FALSE;
                    input_ok=(::SendInput(1,&mouse,sizeof(mouse))==1)&&input_ok;
                    const auto wait_until=::GetTickCount64()+1000;
                    while(!probe.sizing && ::GetTickCount64()<wait_until) ::Sleep(5);
                    input_ok=input_ok && probe.sizing;
                    const auto begin=::GetTickCount64();
                    while(input_ok && ::GetTickCount64()-begin<static_cast<ULONGLONG>(duration) && probe.sizing) {
                        const int phase=static_cast<int>((::GetTickCount64()-begin)%period);
                        const int amount=phase<period/2?phase:period-phase;
                        const int dx=edge.hit==HTCAPTION?amount*200/period:-edge.x*amount*(width-min_width)/(period/2);
                        const int dy=edge.hit==HTCAPTION?amount*100/period:-edge.y*amount*(height-min_height)/(period/2);
                        input_ok=::SetCursorPos(start.x+dx,start.y+dy)!=FALSE;::Sleep(8);
                    }
                    input_ok=input_ok && probe.sizing &&
                        ::GetTickCount64()-begin>=static_cast<ULONGLONG>(duration);
                    if(input_ok && edge.hit!=HTCAPTION) {
                        // Deliberately cross the native minimum, then hold it:
                        // sampled triangle peaks alone can miss the exact limit.
                        input_ok=::SetCursorPos(start.x-edge.x*(width-min_width+dip(20)),
                            start.y-edge.y*(height-min_height+dip(20)))!=FALSE;
                        // Wait for the actual native clamp, not a fixed delay:
                        // pointer motion can be coalesced while earlier paints
                        // finish. All intervening frames remain under observation.
                        const auto minimum_deadline=::GetTickCount64()+1000;
                        do {
                            reached_minimum=Bounds(window);
                            if((!edge.x || reached_minimum.right-reached_minimum.left==min_width) &&
                               (!edge.y || reached_minimum.bottom-reached_minimum.top==min_height)) break;
                            ::Sleep(5);
                        } while(probe.sizing && ::GetTickCount64()<minimum_deadline);
                        // Native minimum clamping leaves the pointer inside the
                        // client if it overshoots. Return to the actual frame
                        // before release so an unrelated button hover is not
                        // compared with the neutral reference image.
                        const RECT minimum=reached_minimum;
                        input_ok=::SetCursorPos(edge.x<0 ? minimum.left+2:edge.x>0 ? minimum.right-2:(minimum.left+minimum.right)/2,
                            edge.y<0 ? minimum.top+2:edge.y>0 ? minimum.bottom-2:(minimum.top+minimum.bottom)/2)!=FALSE && input_ok;
                        ::Sleep(20);
                    }
                    mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP;
                    input_ok=(::SendInput(1,&mouse,sizeof(mouse))==1)&&input_ok;
                    done=true;
                });
                CHECK(WaitFor([&]{return done.load();},5000));input.join();Pump(80);
                CHECK(input_ok && !probe.sizing);
                if(edge.hit!=HTCAPTION) {
                    // Check the native clamp before returning the pointer to
                    // its hit-test band, which can grow the frame by a pixel.
                    std::printf("Native minimum: expected=%dx%d reached=%ldx%ld edge=%ls\n",
                        min_width,min_height,reached_minimum.right-reached_minimum.left,
                        reached_minimum.bottom-reached_minimum.top,edge.name);
                    CHECK(!edge.x || reached_minimum.right-reached_minimum.left==min_width);
                    CHECK(!edge.y || reached_minimum.bottom-reached_minimum.top==min_height);
                }
            } else for(int cycle=0;cycle<4;++cycle) {
                ::SendMessageW(maximize,BM_CLICK,0,0);Pump(250);
                CHECK((::IsZoomed(window)!=FALSE)==(cycle%2==0));
                settled=true;Pump(100);settled=false;
            }
            Pump(150);
            settled=true;Pump(100);settled=false;
        }
        stop=true;observer.join();
        std::printf("All-edge %s %ls: frames=%u mixed_geometry=%u holes=%u capture_errors=%u\n",
            theme==app::AppearanceTheme::Light?"light":"dark",edge.name,frames,bad,holes,errors);
        std::printf("Resize work: count=%u mean_ms=%.2f max_ms=%llu\n",probe.measured_moves,
            probe.measured_moves?static_cast<double>(probe.resize_total)/probe.measured_moves:0.0,
            static_cast<unsigned long long>(probe.resize_max));
        std::printf("Settled frames: %u invalid=%u\n",settled_frames,settled_bad);
        std::printf("Legacy three-patch mixed observations (diagnostic only): %u\n",legacy_bad);
        std::printf("Owned paints: count=%u total_us=%lld max_us=%lld; layout: count=%u total_us=%lld max_us=%lld\n",
            probe.paints,probe.paint_us,probe.paint_max_us,probe.sizes,probe.size_us,probe.size_max_us);
        if(!evidence.empty()) for(const auto& picture:pictures) {
            const auto stem=evidence/(std::wstring(mode)+L"-all-edge-"+std::to_wstring(static_cast<int>(theme))+L"-"+edge.name+L"-"+picture.name);
            CHECK(platform::SaveImage(*picture.image,stem.wstring()+L".png"));
            std::ofstream metadata(stem.wstring()+L".json");
            const auto rect=[&](const RECT& r) { metadata<<'['<<r.left<<','<<r.top<<','<<r.right<<','<<r.bottom<<']'; };
            const auto geometry=[&](const Geometry& g) {
                metadata<<"{\"client\":";rect(g.client);metadata<<",\"patches\":[";
                for(size_t i=0;i<g.patches.size();++i) { if(i) metadata<<',';rect(g.patches[i]); }
                metadata<<"]}";
            };
            metadata<<"{\"work\":";rect(work);metadata<<",\"common\":";rect(picture.common);
            metadata<<",\"reference\":";geometry(picture.reference);metadata<<",\"history\":[";
            for(size_t i=0;i<picture.history.size();++i) { if(i) metadata<<',';geometry(picture.history[i]); }
            metadata<<"]}\n";CHECK(metadata.good());
        }
        pictures.clear();
        const HWND video=::FindWindowExW(window,nullptr,L"EasnapTrimVideo",nullptr);
        DWORD helper_pid=0;if(video) ::GetWindowThreadProcessId(::GetWindow(video,GW_CHILD),&helper_pid);
        for(DWORD pid:{::GetCurrentProcessId(),helper_pid}) if(pid) {
            HANDLE process=::OpenProcess(PROCESS_QUERY_INFORMATION|PROCESS_VM_READ,FALSE,pid);
            CHECK(process!=nullptr);
            if(process) {
                PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);DWORD handles=0;
                CHECK(::GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof(memory)));
                CHECK(::GetProcessHandleCount(process,&handles));
                std::printf("Resize resources %s: pid=%lu GDI=%lu USER=%lu handles=%lu working_set=%zu private=%zu\n",
                    pid==helper_pid ? "helper":"owner",pid,::GetGuiResources(process,GR_GDIOBJECTS),
                    ::GetGuiResources(process,GR_USEROBJECTS),handles,memory.WorkingSetSize,memory.PrivateUsage);
                ::CloseHandle(process);
            }
        }
        if(!external_capture) {
            CHECK(frames>=10 && !bad && !holes && !errors);
            CHECK(settled_frames>0 && settled_bad==0);
        }
        else { std::puts("External-capture diagnostic: no automatic visual result"); }
        CHECK(probe.resize_max<1000);
        const unsigned completed_moves=probe.moves;
        Pump(100);
        CHECK(probe.moves==completed_moves);
        if(playing) {
            wchar_t action[32]{};::GetWindowTextW(::GetDlgItem(window,100),action,32);
            CHECK(std::wstring_view(action)==L"Pause");
            ::SendMessageW(::GetDlgItem(window,100),BM_CLICK,0,0);
        }
    }
    ::RemoveWindowSubclass(window,Probe::Proc,0xEA70);
    ::EnumChildWindows(window,[](HWND child,LPARAM)->BOOL {
        if(::GetWindowThreadProcessId(child,nullptr)==::GetCurrentThreadId()) ::RemoveWindowSubclass(child,Probe::Proc,0xEA70);
        return TRUE;
    },0);
    ::ShowWindow(window,SW_RESTORE);
    ::SetWindowPos(window,HWND_NOTOPMOST,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOACTIVATE);
    ::DestroyWindow(backdrop);::UnregisterClassW(cls.lpszClassName,cls.hInstance);::DeleteObject(brush);
    if(restore_cursor) ::SetCursorPos(old_cursor.x,old_cursor.y);
    Appearance(old_theme);
}
