#include "desktop_test_window.h"
// Real MFPlay transport and presented-frame acceptance, independent of export.
#include <mfapi.h>
#include <mferror.h>
#include <mfplay.h>
#include <dwmapi.h>
#include <psapi.h>
#include <wrl/client.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "media/media_preview.h"
#include "media/preview_video_endpoint.h"
#include "media/media_foundation_writer.h"
#include "app/trim_workflow.h"
#include "capture/capture_provider.h"
#include "preview_test_fixture.h"

namespace {
using namespace easnap::media;
constexpr int64_t kSecond = 10'000'000;
int failures = 0;

bool Check(bool value, const char* label) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", label);
    }
    return value;
}

template<class Predicate>
bool Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    do {
        if (predicate()) return true;
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (std::chrono::steady_clock::now() < deadline);
    return predicate();
}

bool CapturePresentedFrame(easnap::capture::IRegionCaptureProvider& capture,
                           IMediaPreview& preview, const RECT& bounds,
                           VideoFrame& frame, int* status=nullptr) {
    const auto result=capture.CaptureRegionFrame(bounds,false,MediaTime::zero(),
                                                 MediaTime{333'333},frame);
    if (status) *status=static_cast<int>(result.status);
    if (result.status==easnap::capture::RegionCaptureStatus::FrameAcquired)
        return frame.image && frame.image->width==bounds.right-bounds.left &&
            frame.image->height==bounds.bottom-bounds.top;
    const auto state=preview.snapshot();
    if (result.status!=easnap::capture::RegionCaptureStatus::NoNewFrame ||
        state.play_requested || state.native_playing) return false;
    // An unchanged desktop need not produce another duplication frame. Read
    // the actual desktop again through the production screenshot path, whose
    // GDI fallback handles that case; never accept a previously cached image.
    auto current=std::make_shared<easnap::platform::BgraImage>();
    if (!capture.CaptureRegion(bounds,false,*current) ||
        current->width!=bounds.right-bounds.left || current->height!=bounds.bottom-bounds.top) return false;
    frame.image=std::move(current);
    if (status) *status=3; // Fresh one-shot desktop capture after NoNewFrame.
    return true;
}

bool MakeSource(const std::wstring& path) {
    auto writer = CreateMediaFoundationWriter();
    RecordingOptions options;
    options.region = RECT{0, 0, 160, 96};
    options.target_frame_rate = 15;
    options.audio = AudioSource::None;
    if (!writer || !writer->Open(path, options, nullptr)) return false;
    auto image = std::make_shared<easnap::platform::BgraImage>();
    image->width = 160;
    image->height = 96;
    image->pixels.resize(160 * 96);
    for (int64_t index = 0; index < 180; ++index) {
        std::fill(image->pixels.begin(), image->pixels.end(),
                  0xff000000u | (static_cast<uint32_t>(index) * 137u));
        const MediaTime start{index * kSecond / 15};
        const MediaTime end{(index + 1) * kSecond / 15};
        if (!writer->WriteVideoFrame({image, start, end - start})) return false;
    }
    std::wstring finalized;
    return writer->Finalize(finalized);
}

bool MakeOpeningSource(const std::wstring& path, int frame_count) {
    auto writer = CreateMediaFoundationWriter();
    RecordingOptions options;
    options.region = RECT{0,0,160,96};
    options.target_frame_rate = 15;
    options.audio = AudioSource::None;
    if (!writer || !writer->Open(path,options,nullptr)) return false;
    for (int index=0;index<frame_count;++index) {
        auto image=std::make_shared<easnap::platform::BgraImage>();
        image->Reset(160,96);
        // The first frame is green and every later frame blue, so a paused
        // picture at an arbitrary nonzero position cannot satisfy this test.
        std::fill(image->pixels.begin(),image->pixels.end(),
                  index==0 ? 0xff25d05au : 0xff2835d0u);
        if (index==0) {
            // Asymmetric edge bands also detect inverted RGB32 scanlines in
            // the single-frame still surface. The sampled center stays green.
            for (int y=0;y<8;++y) {
                std::fill_n(image->row(y),image->width,0xffd03428u);
                std::fill_n(image->row(image->height-1-y),image->width,0xff2835d0u);
            }
        }
        const MediaTime start{index*kSecond/15};
        const MediaTime end{(index+1)*kSecond/15};
        if (!writer->WriteVideoFrame({image,start,end-start})) return false;
    }
    std::wstring finalized;
    return writer->Finalize(finalized);
}

void ExerciseInitialFrame(const std::filesystem::path& directory, HWND window) {
    std::puts("Preview startup: real first frame, paused at zero, including one-frame recording");
    const RECT placement=easnap::test::OnTestMonitor({120,120,440,312});
    if (!Check(::SetWindowPos(window,HWND_TOPMOST,placement.left,placement.top,320,192,SWP_NOACTIVATE|SWP_SHOWWINDOW)!=FALSE,
               "show owned preview test surface")) return;
    const auto bring_forward=[&] { return easnap::test::DesktopTestWindow::Present(window); };
    // A real, test-owned occluder verifies z-order recovery without bringing
    // the user's browser forward or taking their keyboard focus.
    const HWND occluder=::CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,
        L"STATIC",L"Easnap occlusion probe",WS_POPUP|WS_VISIBLE|SS_NOTIFY|SS_BLACKRECT,placement.left,placement.top,320,192,
        nullptr,nullptr,::GetModuleHandleW(nullptr),nullptr);
    Check(occluder!=nullptr,"independent occluding window created");
    if (occluder) {
        ::SetWindowPos(occluder,HWND_TOPMOST,placement.left,placement.top,320,192,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        Check(::WindowFromPoint(POINT{placement.left+80,placement.top+60})==occluder,"test surface actually occluded");
        Check(bring_forward() && ::GetAncestor(::WindowFromPoint(POINT{placement.left+80,placement.top+60}),GA_ROOT)==window,
              "standalone test surface regains topmost ownership without activation");
        ::DestroyWindow(occluder);
    }
    for (const int frames:{1,6}) {
        const auto source=directory/(L"opening-"+std::to_wstring(frames)+L".mp4");
        if (!Check(MakeOpeningSource(source.wstring(),frames),"opening frame fixture")) continue;
        for (int cycle=0;cycle<4;++cycle) {
            auto preview=CreateMfPlayPreview();
            if (!Check(preview && preview->Open(source.wstring(),window,window,WM_APP+7),
                       "open without user Play")) continue;
            const bool ready=Wait([&] {
                const auto current=preview->snapshot();
                return current.state==MediaPreviewState::Ready || current.state==MediaPreviewState::Failed;
            });
            const auto current=preview->snapshot();
            if (!Check(ready && current.state==MediaPreviewState::Ready,"opening prepares paused first frame")) {
                std::fwprintf(stderr,L"%ls\n",preview->diagnostics().c_str());
                preview->Close();
                continue;
            }
            Check(!current.play_requested && !current.native_playing && !current.command_pending &&
                  !current.position_pending && current.position==MediaTime::zero(),
                  "ready is genuinely settled and paused at zero");
            Check(bring_forward(),"position ready preview surface in front");
            HWND sampled_child=nullptr;
            DWORD sampled_process=0;
            RECT sampled_client{};
            COLORREF sampled_pixels[11]{};
            auto capture=easnap::capture::CreateRegionCaptureProvider();
            int capture_status=-1;
            auto display_generation=easnap::test::DesktopTestWindow::display_generation();
            RECT sampled_screen{};
            std::shared_ptr<const easnap::platform::BgraImage> last_presented;
            HWND sampled_obscurer=nullptr;
            DWORD sampled_cloaked=0;
            const auto first_frame_visible=[&] {
                bring_forward();
                preview->UpdateVideo();
                if (display_generation != easnap::test::DesktopTestWindow::display_generation()) {
                    capture=easnap::capture::CreateRegionCaptureProvider();
                    display_generation=easnap::test::DesktopTestWindow::display_generation();
                }
                const HWND child=::GetWindow(window,GW_CHILD);
                DWORD process=0; ::GetWindowThreadProcessId(child,&process);
                sampled_child=child;
                sampled_process=process;
                sampled_client={};
                sampled_obscurer=nullptr;
                sampled_cloaked=0;
                for (auto& pixel:sampled_pixels) pixel=CLR_INVALID;
                if (!child || process!=preview->snapshot().backend_process_id || !::IsWindowVisible(child)) return false;
                RECT client{}; ::GetClientRect(child,&client);
                sampled_client=client;
                RECT target{}; ::GetClientRect(window,&target);
                if (client.right!=target.right || client.bottom!=target.bottom) return false;
                RECT screen=client;
                ::MapWindowPoints(child,nullptr,reinterpret_cast<POINT*>(&screen),2);
                sampled_screen=screen;
                const HRESULT cloak_result=::DwmGetWindowAttribute(window,DWMWA_CLOAKED,
                    &sampled_cloaked,sizeof(sampled_cloaked));
                bool owned=::IsWindowVisible(window) && SUCCEEDED(cloak_result) && !sampled_cloaked;
                const auto owns_point=[&](int x,int y) {
                    const HWND hit=::WindowFromPoint(POINT{screen.left+x,screen.top+y});
                    const bool ours=hit && ::GetAncestor(hit,GA_ROOT)==window;
                    if (!ours && !sampled_obscurer) sampled_obscurer=hit;
                    return ours;
                };
                if (!owned || !owns_point(client.right/2,client.bottom/2)) {
                    bring_forward();
                    return false;
                }
                VideoFrame presented;
                if (!CapturePresentedFrame(*capture,*preview,screen,presented,&capture_status)) return false;
                last_presented=presented.image;
                bool green=true;
                int index=0;
                for (int y:{client.bottom/3,client.bottom/2,2*client.bottom/3}) {
                    for (int x:{client.right/3,client.right/2,2*client.right/3}) {
                        owned=owns_point(x,y) && owned;
                        const auto bgra=presented.image->row(y)[x];
                        const COLORREF pixel=RGB((bgra>>16)&0xff,(bgra>>8)&0xff,bgra&0xff);
                        sampled_pixels[index++]=pixel;
                        green=green && pixel!=CLR_INVALID && GetGValue(pixel)>150 &&
                            GetRValue(pixel)<90 && GetBValue(pixel)<130;
                    }
                }
                const auto top=presented.image->row(client.bottom/24)[client.right/2];
                const auto bottom=presented.image->row(client.bottom-1-client.bottom/24)[client.right/2];
                sampled_pixels[9]=RGB((top>>16)&0xff,(top>>8)&0xff,top&0xff);
                sampled_pixels[10]=RGB((bottom>>16)&0xff,(bottom>>8)&0xff,bottom&0xff);
                owned=owns_point(client.right/2,client.bottom/24) && owned;
                owned=owns_point(client.right/2,client.bottom-1-client.bottom/24) && owned;
                green=green && GetRValue(sampled_pixels[9])>150 && GetGValue(sampled_pixels[9])<100 &&
                    GetBValue(sampled_pixels[9])<130 && GetBValue(sampled_pixels[10])>150 &&
                    GetRValue(sampled_pixels[10])<100 && GetGValue(sampled_pixels[10])<130;
                return green && owned;
            };
            const bool first_visible=Wait(first_frame_visible);
            if (!Check(first_visible, sampled_obscurer
                    ? "ENVIRONMENT BLOCKED: another window covers the desktop sample"
                    : "first decoded green frame is actually presented")) {
                const auto failed=preview->snapshot();
                HDC child_dc=sampled_child?::GetDC(sampled_child):nullptr;
                const COLORREF child_dc_pixel=child_dc?::GetPixel(child_dc,
                    sampled_client.right/2,sampled_client.bottom/2):CLR_INVALID;
                if (child_dc) ::ReleaseDC(sampled_child,child_dc);
                std::printf("Startup pixels: frames=%d cycle=%d child=%p process=%lu expected=%lu visible=%d client=%ldx%ld native_playing=%d logical_ms=%lld native_ms=%lld\n",
                    frames,cycle,static_cast<void*>(sampled_child),sampled_process,static_cast<unsigned long>(failed.backend_process_id),
                    ::IsWindowVisible(sampled_child),sampled_client.right,sampled_client.bottom,
                    failed.native_playing,static_cast<long long>(failed.position.count()),
                    static_cast<long long>(failed.native_position.count()));
                for (const auto pixel:sampled_pixels) std::printf(" %08lx",pixel);
                std::printf(" child_dc=%08lx capture_status=%d\n",child_dc_pixel,capture_status);
                DWORD obscuring_process=0;
                ::GetWindowThreadProcessId(sampled_obscurer,&obscuring_process);
                wchar_t obscuring_class[128]{};
                ::GetClassNameW(sampled_obscurer,obscuring_class,128);
                std::printf("Startup visibility: foreground=%p obscurer=%p root=%p pid=%lu cloak=%lu topmost=%d\n",
                    static_cast<void*>(::GetForegroundWindow()),static_cast<void*>(sampled_obscurer),
                    static_cast<void*>(::GetAncestor(sampled_obscurer,GA_ROOT)),obscuring_process,
                    sampled_cloaked,(::GetWindowLongPtrW(window,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0);
                std::wprintf(L"Startup obscuring window class: %ls\n",obscuring_class);
                const HWND still=::GetWindow(sampled_child,GW_CHILD);
                RECT still_rect{}; ::GetWindowRect(still,&still_rect);
                HDC still_dc=still?::GetDC(still):nullptr;
                const COLORREF still_center=still_dc?::GetPixel(still_dc,
                    (still_rect.right-still_rect.left)/2,(still_rect.bottom-still_rect.top)/2):CLR_INVALID;
                if (still_dc) ::ReleaseDC(still,still_dc);
                HDC desktop=::GetDC(nullptr);
                const COLORREF desktop_center=desktop?::GetPixel(desktop,
                    (sampled_screen.left+sampled_screen.right)/2,
                    (sampled_screen.top+sampled_screen.bottom)/2):CLR_INVALID;
                std::printf("Startup capture: parent_dpi=%u child_dpi=%u screen=(%ld,%ld)-(%ld,%ld) still=%p visible=%d rect=(%ld,%ld)-(%ld,%ld) still_center=%08lx desktop_center=%08lx\n",
                    ::GetDpiForWindow(window),::GetDpiForWindow(sampled_child),sampled_screen.left,sampled_screen.top,
                    sampled_screen.right,sampled_screen.bottom,static_cast<void*>(still),::IsWindowVisible(still),
                    still_rect.left,still_rect.top,still_rect.right,still_rect.bottom,still_center,desktop_center);
                const auto evidence=directory.parent_path()/(directory.filename().wstring()+L"-evidence");
                std::error_code evidence_error;
                std::filesystem::create_directory(evidence,evidence_error);
                const auto prefix=L"frames-"+std::to_wstring(frames)+L"-cycle-"+std::to_wstring(cycle);
                if (!evidence_error && last_presented) {
                    easnap::platform::SaveImage(*last_presented,(evidence/(prefix+L"-provider.png")).wstring());
                    easnap::platform::BgraImage gdi;
                    gdi.Reset(last_presented->width,last_presented->height);
                    {
                        easnap::platform::GdiImageSurface surface(gdi);
                        if (desktop && surface.valid()) {
                            ::BitBlt(surface.dc(),0,0,gdi.width,gdi.height,desktop,
                                     sampled_screen.left,sampled_screen.top,SRCCOPY|CAPTUREBLT);
                        }
                    }
                    gdi.MakeOpaque();
                    easnap::platform::SaveImage(gdi,(evidence/(prefix+L"-desktop-gdi.png")).wstring());
                    // stdout mixes printf and wprintf here; a write in the other
                    // orientation is dropped, so follow the orientation it has.
                    if (std::fwide(stdout,0)>0) std::wprintf(L"Startup actual-screen evidence: %ls, provider=%ls\n",evidence.c_str(),capture->name());
                    else std::printf("Startup actual-screen evidence: %ls, provider=%ls\n",evidence.c_str(),capture->name());
                }
                if (desktop) ::ReleaseDC(nullptr,desktop);
                std::fwprintf(stderr,L"%ls\n",preview->diagnostics().c_str());
            }
            if (!first_visible && sampled_obscurer) {
                preview->Close();
                std::error_code cleanup;
                std::filesystem::remove(source,cleanup);
                return; // report the real visibility failure, not accumulated 60 s waits
            }
            const auto until=::GetTickCount64()+350;
            Wait([&] { return ::GetTickCount64()>=until; });
            const auto settled=preview->snapshot();
            Check(settled.state==MediaPreviewState::Ready && !settled.native_playing &&
                  !settled.play_requested && settled.position==MediaTime::zero(),
                  "startup stays paused at zero without autoplay");
            // A paused desktop may have no newly captured frame on this poll,
            // or another foreground window may temporarily obscure the test.
            // Wait for an actual visible sample while requiring the transport
            // to remain paused at zero on every poll; never accept cached pixels.
            bool remained_paused=true;
            const bool visible_again=Wait([&] {
                const auto state=preview->snapshot();
                remained_paused=state.state==MediaPreviewState::Ready && !state.native_playing &&
                    !state.play_requested && state.position==MediaTime::zero();
                return !remained_paused || first_frame_visible();
            });
            if(!Check(visible_again && remained_paused,"paused first frame remains presented")) {
                std::printf("Startup repeat sample: frames=%d cycle=%d paused=%d capture_status=%d obscurer=%p\n",
                    frames,cycle,remained_paused,capture_status,static_cast<void*>(sampled_obscurer));
                std::fwprintf(stderr,L"%ls\n",preview->diagnostics().c_str());
            }
            if (frames==1 && cycle==0) {
                ::SetWindowPos(window,nullptr,0,0,400,240,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                preview->UpdateVideo();
                Check(Wait(first_frame_visible),"single-frame picture resizes with upright edge bands");
                Check(preview->PauseAt(current.duration),"single-frame endpoint seek accepted");
                Check(Wait([&] {
                    const auto at_end=preview->snapshot();
                    return at_end.state!=MediaPreviewState::Failed && !at_end.command_pending &&
                        !at_end.position_pending && at_end.position==current.duration;
                }) && first_frame_visible(),"single-frame endpoint preserves its actual picture");
                ::SetWindowPos(window,nullptr,0,0,320,192,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                preview->UpdateVideo();
            }
            preview->Close();
        }
        std::error_code error; std::filesystem::remove(source,error);
        Check(!error,"opening fixture released");
    }
    ::ShowWindow(window,SW_HIDE);
    ::SetWindowPos(window,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
}


bool MakeEndpointSource(const std::wstring& path, int frames, int rate, int width, int height) {
    auto writer=CreateMediaFoundationWriter();
    RecordingOptions options;
    options.region=RECT{0,0,width,height}; options.target_frame_rate=rate; options.audio=AudioSource::None;
    if (!writer || !writer->Open(path,options,nullptr)) return false;
    constexpr uint32_t colors[]{0xffd03028,0xff25d05a,0xffd0cf28,0xff28cfd0,0xffd028c0,0xff2835d0};
    for (int index=0;index<frames;++index) {
        auto frame=std::make_shared<easnap::platform::BgraImage>(); frame->Reset(width,height);
        // A desktop observer can legitimately miss a single 16.7 ms frame.
        // At 60 fps make the initial red scene uniquely identifiable for 100 ms
        // (it never recurs), while keeping the final blue sample one frame long.
        const uint32_t color = rate == 60
            ? (index < 6 ? colors[0] : index == frames-1 ? colors[5] : colors[1+(index-6)%5])
            : colors[index%6];
        std::fill(frame->pixels.begin(),frame->pixels.end(),color);
        const MediaTime start{index*kSecond/rate},end{(index+1)*kSecond/rate};
        if (!writer->WriteVideoFrame({frame,start,end-start})) return false;
    }
    std::wstring finalized; return writer->Finalize(finalized);
}

void ExerciseRetainedEndpoints(const std::filesystem::path& directory, HWND window) {
    std::puts("Retained endpoints: real desktop pixels, natural EOS, half-open Out and replay from In");
    Check(easnap::test::DesktopTestWindow::Present(window),"standalone endpoint test surface");
    struct Fixture { int frames, rate, width, height; };
    for (const auto [frames,rate,width,height]:{Fixture{1,5,160,96},Fixture{6,5,160,96},
            Fixture{18,60,160,96},Fixture{6,5,1920,1080},Fixture{6,5,364,490}}) {
        const auto path=directory/(L"endpoint-"+std::to_wstring(frames)+L".mp4");
        if (!Check(MakeEndpointSource(path.wstring(),frames,rate,width,height),"colored endpoint fixture")) return;
        auto preview=CreateMfPlayPreview();
        if (!Check(preview && preview->Open(path.wstring(),window,window,WM_APP+7),"endpoint source opens")) return;
        if (!Check(Wait([&] { return preview->snapshot().state==MediaPreviewState::Ready; }),"endpoint source ready")) return;
        auto capture=easnap::capture::CreateRegionCaptureProvider();
        auto display=easnap::test::DesktopTestWindow::display_generation();
        bool black_observed=false;
        unsigned colored_samples=0;
        const auto picture=[&](COLORREF expected, bool image_edges=false) {
            easnap::test::DesktopTestWindow::Present(window); preview->UpdateVideo();
            if (display!=easnap::test::DesktopTestWindow::display_generation()) {
                capture=easnap::capture::CreateRegionCaptureProvider();
                display=easnap::test::DesktopTestWindow::display_generation();
            }
            RECT bounds{}; ::GetClientRect(window,&bounds);
            ::MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&bounds),2);
            const HWND hit=::WindowFromPoint(POINT{(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2});
            if (!hit || ::GetAncestor(hit,GA_ROOT)!=window) return false;
            VideoFrame frame;
            if (!CapturePresentedFrame(*capture,*preview,bounds,frame)) return false;
            RECT current_bounds{}; ::GetClientRect(window,&current_bounds);
            ::MapWindowPoints(window,nullptr,reinterpret_cast<POINT*>(&current_bounds),2);
            const HWND current_hit=::WindowFromPoint(POINT{(bounds.left+bounds.right)/2,(bounds.top+bounds.bottom)/2});
            if (!::EqualRect(&bounds,&current_bounds) || !current_hit ||
                ::GetAncestor(current_hit,GA_ROOT)!=window) return false;
            const double scale=(std::min)(static_cast<double>(frame.image->width)/width,
                                           static_cast<double>(frame.image->height)/height);
            const int shown_width=static_cast<int>(width*scale),shown_height=static_cast<int>(height*scale);
            const int left=(frame.image->width-shown_width)/2,top=(frame.image->height-shown_height)/2;
            // Include the last visible image rows, where H.264 alignment padding
            // leaked into 1080p and other non-macroblock-aligned stills.
            // Held stills have exact integer destination bounds. Check their
            // first/last visible pixels: even one leaked padding row matters.
            // Native video can round a fractional letterbox boundary differently.
            const int first=image_edges?0:2, last=image_edges?1:3;
            for (int y:{top+first,top+shown_height/2,top+shown_height-last}) {
                for (int x:{left+first,left+shown_width/2,left+shown_width-last}) {
                    const auto pixel=frame.image->row(y)[x];
                    if (expected==CLR_INVALID) {
                        if (((pixel>>16)&255)+((pixel>>8)&255)+(pixel&255)<60) {
                            if (!black_observed) {
                                const auto state=preview->snapshot();
                                std::printf("Black transition: ms=%llu frames=%d size=%dx%d state=%d logical=%lld native=%lld pending=%d playing=%d\n",
                                    ::GetTickCount64(),frames,width,height,static_cast<int>(state.state),state.position.count(),state.native_position.count(),
                                    state.command_pending,state.native_playing);
                                const auto evidence=directory.parent_path()/(directory.filename().wstring()+L"-evidence");
                                std::filesystem::create_directories(evidence);
                                const auto file=evidence/(L"black-"+std::to_wstring(frames)+L".png");
                                easnap::platform::SaveImage(*frame.image,file.wstring());
                                std::wprintf(L"Black transition desktop capture: %ls\n",file.c_str());
                                const HWND host=::GetWindow(window,GW_CHILD);
                                for (HWND child=::GetWindow(host,GW_CHILD);child;child=::GetWindow(child,GW_HWNDNEXT)) {
                                    wchar_t name[128]{}; ::GetClassNameW(child,name,128);
                                    HDC dc=::GetDC(child); RECT size{}; ::GetClientRect(child,&size);
                                    const auto native_pixel=::GetPixel(dc,size.right/2,size.bottom/2); ::ReleaseDC(child,dc);
                                    std::wprintf(L"Child %p class=%ls visible=%d pixel=%08lx\n",static_cast<void*>(child),name,::IsWindowVisible(child),native_pixel);
                                }
                            }
                            black_observed=true;
                        }
                        else ++colored_samples;
                        continue;
                    }
                    if (std::abs(static_cast<int>((pixel>>16)&255)-GetRValue(expected))>45 ||
                        std::abs(static_cast<int>((pixel>>8)&255)-GetGValue(expected))>45 ||
                        std::abs(static_cast<int>(pixel&255)-GetBValue(expected))>45) return false;
                }
            }
            return true;
        };
        const auto held=[&](MediaTime position,COLORREF expected) {
            bool correct=Check(Wait([&] {
                const auto state=preview->snapshot();
                return !state.play_requested && !state.native_playing && !state.command_pending &&
                    state.position==position;
            }),"endpoint keeps logical cursor and stopped transport");
            correct=Check(Wait([&] { return picture(expected,true); }),"actual selected last frame visible") && correct;
            const auto until=::GetTickCount64()+350;
            bool stable=true;
            Wait([&] {
                const auto state=preview->snapshot();
                stable=stable && !state.play_requested && !state.native_playing && state.position==position;
                return ::GetTickCount64()>=until;
            });
            correct=Check(stable && Wait([&] { return picture(expected,true); }),"last frame and cursor persist until user input") && correct;
            return correct;
        };
        const auto duration=preview->snapshot().duration;
        Check(Wait([&] { return picture(RGB(208,48,40)); }),"natural playback starts with first picture presented");
        Check(preview->Play(),"natural playback starts");
        Check(Wait([&] {
            picture(CLR_INVALID);
            return preview->snapshot().state==MediaPreviewState::Ended;
        }),"natural playback reaches EOS");
        Check(colored_samples>0 && !black_observed,"actual content stays visible through natural completion");
        held(duration,frames==1?RGB(208,48,40):RGB(40,53,208));
        ::SetWindowPos(window,nullptr,0,0,300,240,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        preview->UpdateVideo();
        Check(Wait([&] { return picture(frames==1?RGB(208,48,40):RGB(40,53,208),true); }),
              "retained final frame survives aspect-ratio resize");
        ::SetWindowPos(window,nullptr,0,0,320,192,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        preview->UpdateVideo();
        Check(Wait([&] { return picture(frames==1?RGB(208,48,40):RGB(40,53,208),true); }),
              "restored viewport is presented before replay observation");
        const auto prior_command=preview->snapshot().command_sequence;
        Check(preview->Play(),"replay accepted from held EOS");
        Check(Wait([&] { return picture(RGB(208,48,40)); }),"replay shows first source frame");
        Check(Wait([&] { return preview->snapshot().command_sequence>prior_command; }),"replay issues real native transport");
        Check(preview->Pause(),"pause replay");
        Check(Wait([&] { const auto state=preview->snapshot(); return !state.command_pending && !state.native_playing; }),"replay pauses");
        if (frames==6 && width==160) {
            for (bool aligned:{true,false}) {
                const MediaTime in{aligned?2*kSecond/10:45*kSecond/100};
                const MediaTime out{aligned?6*kSecond/10:65*kSecond/100};
                const COLORREF first=aligned?RGB(37,208,90):RGB(208,207,40);
                const COLORREF last=aligned?RGB(208,207,40):RGB(40,207,208);
                Check(preview->PlayFrom(in),"selected range starts at In");
                Check(Wait([&] { return picture(first); }),"first frame covers selected In");
                Check(Wait([&] {
                    const auto state=preview->snapshot();
                    const auto decision=easnap::app::PlanTrimPlaybackProgress(
                        (state.play_requested || state.state==MediaPreviewState::Ended) && !state.position_pending,
                        state.position,out);
                    if (decision.action==easnap::app::TrimPlaybackAction::PauseAt)
                        Check(preview->PauseAt(decision.position),"production Out decision accepted");
                    return !state.command_pending && !state.play_requested && state.position==out;
                }),"selected playback settles exactly at Out");
                held(out,last);
                const auto state=preview->snapshot();
                const auto replay=easnap::app::PlanTrimPlaybackToggle(state.play_requested,
                    state.state==MediaPreviewState::Ended,state.position,in,out);
                Check(replay.action==easnap::app::TrimPlaybackAction::SeekThenPlay && replay.position==in,
                    "selected replay plans In seek then Play");
                Check(preview->PlayFrom(replay.position),"selected replay accepted");
                Check(Wait([&] { return picture(first); }),"selected replay starts with correct first frame");
                preview->Pause();
                Check(Wait([&] { return !preview->snapshot().command_pending; }),"selected replay pause settles");
            }
        }
        if (failures) std::fwprintf(stderr,L"%ls\n",preview->diagnostics().c_str());
        preview->Close(); std::filesystem::remove(path);
    }
}

class NativeProbe final : public IMFPMediaPlayerCallback {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (id != __uuidof(IUnknown) && id != __uuidof(IMFPMediaPlayerCallback))
            return E_NOINTERFACE;
        *value = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (!left) delete this;
        return left;
    }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        MFP_MEDIAPLAYER_STATE native = MFP_MEDIAPLAYER_STATE_EMPTY;
        event->pMediaPlayer->GetState(&native);
        std::printf("native event=%u hr=%08X header-state=%u current-state=%u\n",
                    event->eEventType, static_cast<unsigned>(event->hrEvent),
                    event->eState, native);
        if (event->eEventType < 13) {
            results[event->eEventType] = event->hrEvent;
            ++events[event->eEventType];
        }
    }
    std::atomic<unsigned> events[13]{};
    std::atomic<HRESULT> results[13]{};
private:
    std::atomic<ULONG> references_{1};
};

void ProbeNative(const std::wstring& path, HWND window, bool audio_fixture = false) {
    Microsoft::WRL::ComPtr<NativeProbe> callback;
    callback.Attach(new NativeProbe());
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player;
    if (!Check(SUCCEEDED(::MFPCreateMediaPlayer(path.c_str(), FALSE,
            MFP_OPTION_FREE_THREADED_CALLBACK, callback.Get(), window,
            &player)), "native open")) return;
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_MEDIAITEM_SET] > 0; }),
          "native ready");
    Microsoft::WRL::ComPtr<IMFPMediaItem> item;
    PROPVARIANT duration{};
    if (!Check(SUCCEEDED(player->GetMediaItem(&item)) && item &&
                   SUCCEEDED(item->GetDuration(MFP_POSITIONTYPE_100NS, &duration)),
               "native presentation duration")) { player->Shutdown(); return; }
    const MediaTime presentation_duration{duration.vt == VT_UI8
        ? static_cast<int64_t>(duration.uhVal.QuadPart) : duration.hVal.QuadPart};
    ::PropVariantClear(&duration);
    PreviewVideoEndpoint endpoint;
    if (!Check(SUCCEEDED(ReadPreviewVideoEndpoint(path, presentation_duration, endpoint)),
               "read exact final video sample metadata")) { player->Shutdown(); return; }
    Check(endpoint.last_frame_position == MediaTime{
              audio_fixture ? 359 * kSecond / 30 : 179 * kSecond / 15},
          "last video PTS matches generated fixture timestamps");
    Check(endpoint.video_end >= MediaTime{12 * kSecond - 1} &&
              endpoint.video_end <= MediaTime{12 * kSecond},
          "video endpoint derives from the final sample duration");
    Check(endpoint.samples_examined > 0 && endpoint.samples_examined < (audio_fixture ? 360u : 180u),
          "endpoint metadata reads the final GOP without a whole-source scan");
    if (audio_fixture)
        Check(presentation_duration > endpoint.video_end,
              "AAC presentation tail remains distinct from final video sample");
    std::printf("native Pause while stopped returns %08X\n",
                static_cast<unsigned>(player->Pause()));
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_PAUSE] > 0 ||
                           callback->events[MFP_EVENT_TYPE_ERROR] > 0; }),
          "stopped pause result");
    Check(callback->results[MFP_EVENT_TYPE_PAUSE] == MF_E_SESSION_PAUSEWHILESTOPPED,
          "stopped Pause uses the exact asynchronous HRESULT");
    callback->events[MFP_EVENT_TYPE_PAUSE] = 0;
    const auto seek = [&](int64_t position) {
        PROPVARIANT value{};
        value.vt = VT_I8;
        value.hVal.QuadPart = position;
        const unsigned before = callback->events[MFP_EVENT_TYPE_POSITION_SET];
        const HRESULT hr = player->SetPosition(MFP_POSITIONTYPE_100NS, &value);
        std::printf("native seek=%lld hr=%08X\n", position,
                    static_cast<unsigned>(hr));
        if (!Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_POSITION_SET] > before; }),
                   "native seek completes")) return false;
        PROPVARIANT current{};
        player->GetPosition(MFP_POSITIONTYPE_100NS, &current);
        std::printf("native acknowledged seek=%lld GetPosition=%lld\n",
                    position, current.hVal.QuadPart);
        ::PropVariantClear(&current);
        return true;
    };
    if (!seek(6 * kSecond)) { player->Shutdown(); return; }
    player->Play();
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_PLAY] > 0; }), "native play");
    const unsigned prior_play = callback->events[MFP_EVENT_TYPE_PLAY];
    Check(SUCCEEDED(player->Play()), "redundant native Play accepted");
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_PLAY] > prior_play; }),
          "redundant native Play still reports completion");
    player->Pause();
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_PAUSE] > 0; }), "native pause");
    const unsigned prior_pause = callback->events[MFP_EVENT_TYPE_PAUSE];
    Check(SUCCEEDED(player->Pause()), "redundant native Pause accepted");
    Check(Wait([&] { return callback->events[MFP_EVENT_TYPE_PAUSE] > prior_pause; }),
          "redundant native Pause still reports completion");
    // A missing terminal callback leaves ownership ambiguous: stop this direct
    // probe instead of issuing another native command after its timeout. Verify
    // actual SetPosition completions at the exact final PTS from production
    // metadata; the exclusive AAC endpoint is covered through the adapter.
    if (seek(6 * kSecond) && seek(6 * kSecond) &&
        seek(endpoint.last_frame_position.count()) &&
        seek(endpoint.last_frame_position.count()))
        static_cast<void>(seek(0));
    player->Shutdown();
}

void ExercisePreview(const std::wstring& path, HWND window) {
    auto preview = CreateMfPlayPreview();
    if (!Check(preview && preview->Open(path, window, window, WM_APP + 7),
               "preview open")) return;
    const auto settled = [&](bool playing) {
        const bool done = Wait([&] {
            const auto state = preview->snapshot();
            return state.state == MediaPreviewState::Failed ||
                   (!state.command_pending && state.play_requested == playing &&
                    (playing ? state.state == MediaPreviewState::Playing :
                               (state.state == MediaPreviewState::Paused ||
                                (state.state == MediaPreviewState::Ended &&
                                 state.position == state.duration))));
        });
        const auto state = preview->snapshot();
        if (!done || state.state == MediaPreviewState::Failed) {
            std::printf("preview state=%u pending=%u desired=%u error=%08X pos=%lld\n",
                        static_cast<unsigned>(state.state), state.command_pending,
                        state.play_requested, static_cast<unsigned>(state.error),
                        preview->position().count());
        }
        return done && state.state != MediaPreviewState::Failed;
    };
    if (!Check(Wait([&] { return preview->snapshot().state == MediaPreviewState::Ready; }),
               "preview ready")) return;
    Check(preview->PlayFrom(MediaTime{6 * kSecond}), "play from middle");
    if (!Check(settled(true), "middle play settles")) return;
    for (unsigned cycle = 0; cycle < 100; ++cycle) {
        Check(preview->Pause(), "rapid pause");
        Check(preview->Play(), "rapid play");
    }
    if (!Check(settled(true), "rapid toggles settle playing")) return;
    Check(preview->PauseAt(preview->snapshot().duration), "pause at duration");
    if (!Check(settled(false), "end pause settles")) return;
    Check(preview->position() == preview->snapshot().duration,
          "PauseAt end keeps the acknowledged Out position");
    Check(preview->PlayFrom(MediaTime::zero()), "restart");
    Check(preview->Pause(), "cancel restart");
    Check(preview->Play(), "restart last intent");
    Check(settled(true), "restart settles playing");
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        Check(preview->PlayFrom(preview->snapshot().duration - MediaTime{3 * kSecond / 10}),
              "near-end play");
        if (!Check(settled(true), "near-end playing")) break;
        if (!Check(Wait([&] { return preview->snapshot().state == MediaPreviewState::Ended; }),
                   "natural end")) break;
        Check(preview->PauseAt(preview->snapshot().duration), "out pause");
        if (!Check(settled(false), "natural end Out settles")) break;
        Check(preview->PlayFrom(MediaTime::zero()), "out restart");
        Check(preview->Pause(), "out double-toggle");
        Check(preview->Play(), "out last play");
        if (!Check(settled(true), "out restart converges")) break;
    }
    // The same progress planner as TrimWindow, exercised while seeks and
    // automatic Out pauses are pending. No timer positions are fabricated.
    const MediaTime selected_out{8 * kSecond};
    const auto tick = [&] {
        const auto state = preview->snapshot();
        const auto action = easnap::app::PlanTrimPlaybackProgress(
            state.play_requested && !state.position_pending,
            state.position, selected_out);
        if (action.action == easnap::app::TrimPlaybackAction::PauseAt)
            Check(preview->PauseAt(action.position), "automatic selected Out");
    };
    for (unsigned cycle = 0; cycle < 30; ++cycle) {
        Check(preview->PlayFrom(selected_out - MediaTime{kSecond / 10}),
              "play toward selected Out");
        if (!Check(Wait([&] {
            tick();
            const auto state = preview->snapshot();
            return !state.play_requested && !state.command_pending &&
                   state.position == selected_out;
        }), "selected Out pause settles at Out")) return;
        Check(preview->PlayFrom(MediaTime{kSecond}), "restart from selected Out");
        tick(); // old native Out must not overwrite this newer In seek
        Check(preview->Pause(), "double-toggle selected restart");
        Check(preview->Play(), "last selected intent is Play");
        if (!Check(Wait([&] {
            tick();
            const auto state = preview->snapshot();
            return state.play_requested && !state.command_pending &&
                   state.state == MediaPreviewState::Playing &&
                   state.position < selected_out;
        }), "selected Out restart converges")) return;
    }
    Check(preview->PauseAt(MediaTime{5 * kSecond}), "range Out moved behind playhead");
    Check(settled(false), "range Out edit settles");
    Check(preview->Seek(MediaTime{2 * kSecond}), "paused seek");
    Check(settled(false), "paused seek settles");
    Check(preview->position() == MediaTime{2 * kSecond}, "paused seek position");
    Check(preview->Play(), "resume for In edit");
    Check(settled(true), "resume settles");
    Check(preview->Seek(MediaTime{4 * kSecond}), "In ahead while playing");
    Check(settled(true), "active seek preserves Play");
    preview->Close();
    std::printf("Real preview: 100 middle toggles, 30 natural-end and 30 selected-Out restarts\n");
}

// Real-time input driver: intent changes deliberately occur without awaiting
// native completion. Pumping messages delivers the same callbacks and 100 ms
// progress policy that TrimWindow uses, including its selected-Out PauseAt.
class SuspendedPreviewHost final {
public:
    SuspendedPreviewHost() = default;
    SuspendedPreviewHost(const SuspendedPreviewHost&) = delete;
    SuspendedPreviewHost& operator=(const SuspendedPreviewHost&) = delete;
    ~SuspendedPreviewHost() {
        if (resume_cancel_) ::SetEvent(resume_cancel_);
        if (resume_failsafe_.joinable()) resume_failsafe_.join();
        if (thread_ != nullptr) {
            if (suspended_.exchange(false) && ::WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0)
                static_cast<void>(::ResumeThread(thread_));
            ::CloseHandle(thread_);
        }
        if (process_ != nullptr) ::CloseHandle(process_);
        if (resume_cancel_) ::CloseHandle(resume_cancel_);
    }

    bool Suspend(DWORD process_id, DWORD thread_id) {
        // IDs come from this preview's child handshake. Never suspend the test
        // process or an unrelated thread supplied from outside the preview.
        if (!process_id || !thread_id || process_id == ::GetCurrentProcessId()) return false;
        process_ = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                FALSE, process_id);
        thread_ = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_QUERY_LIMITED_INFORMATION |
                                  SYNCHRONIZE,
                              FALSE, thread_id);
        if (!process_ || !thread_ || ::GetProcessIdOfThread(thread_) != process_id) return false;
        resume_cancel_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!resume_cancel_) return false;
        const DWORD previous = ::SuspendThread(thread_);
        if (previous == static_cast<DWORD>(-1)) return false;
        suspended_ = true;
        // Bound the test itself if a regression synchronously enters the
        // suspended host while resizing or closing. Healthy watchdog retirement
        // finishes well before this owned-thread failsafe can run.
        resume_failsafe_ = std::thread([this] {
            if (::WaitForSingleObject(resume_cancel_, 10'000) == WAIT_TIMEOUT &&
                suspended_.exchange(false) &&
                ::WaitForSingleObject(thread_, 0) != WAIT_OBJECT_0)
                static_cast<void>(::ResumeThread(thread_));
        });
        return previous == 0;
    }

    [[nodiscard]] bool Retired() const {
        return process_ && thread_ &&
               ::WaitForSingleObject(process_, 0) == WAIT_OBJECT_0 &&
               ::WaitForSingleObject(thread_, 0) == WAIT_OBJECT_0;
    }

private:
    HANDLE process_{nullptr};
    HANDLE thread_{nullptr};
    HANDLE resume_cancel_{nullptr};
    std::atomic<bool> suspended_{false};
    std::thread resume_failsafe_;
};

bool OwnedVideoHasSize(HWND parent, DWORD process_id, int width, int height) {
    for (HWND child = ::GetWindow(parent, GW_CHILD); child;
         child = ::GetWindow(child, GW_HWNDNEXT)) {
        DWORD child_process_id = 0;
        ::GetWindowThreadProcessId(child, &child_process_id);
        if (child_process_id != process_id) continue;
        RECT bounds{};
        return ::GetClientRect(child, &bounds) &&
               bounds.right - bounds.left == width && bounds.bottom - bounds.top == height;
    }
    return false;
}

class HumanTransportStress final {
public:
    HumanTransportStress(IMediaPreview& preview, uint32_t seed)
        : preview_(preview), seed_(seed), random_(seed) {}

    bool RunContinuousSeek(unsigned seconds) {
        // Unlike the mixed-intent stress below, never pause or reach Out while
        // random input is active. Every target leaves at least 1.5 s before the
        // real end, exceeding the longest input interval of 450 ms.
        selected_in_ = MediaTime::zero();
        selected_out_ = preview_.snapshot().duration;
        policy_enabled_ = false;
        if (selected_out_ < MediaTime{6 * kSecond})
            return Fail("continuous seek fixture is at least six seconds");
        if (!Issue(preview_.PlayFrom(MediaTime{kSecond}), "continuous-initial-play") ||
            !Advancing()) return false;
        const auto original = preview_.snapshot();
        continuous_generation_ = original.player_generation;
        continuous_process_ = original.backend_process_id;
        continuous_active_ = true;

        struct Resources {
            DWORD handles{};
            SIZE_T private_bytes{};
            DWORD gdi{};
            DWORD user{};
        };
        const auto resources = [&](Resources& sample) {
            HANDLE process = ::OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                                           FALSE, continuous_process_);
            if (!process) return Fail("open the owned helper for resource measurement");
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof(memory);
            const bool measured = ::GetProcessHandleCount(process, &sample.handles) &&
                ::K32GetProcessMemoryInfo(process,
                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
            sample.private_bytes = memory.PrivateUsage;
            sample.gdi = ::GetGuiResources(process, GR_GDIOBJECTS);
            sample.user = ::GetGuiResources(process, GR_USEROBJECTS);
            ::CloseHandle(process);
            if (!measured) return Fail("measure owned helper handles and private memory");
            std::printf("Continuous MFPlay seed=%08X resources: handles=%lu private=%llu "
                        "GDI=%lu USER=%lu\n", seed_, sample.handles,
                        static_cast<unsigned long long>(sample.private_bytes),
                        sample.gdi, sample.user);
            return true;
        };
        Resources baseline{};
        if (!resources(baseline)) return false;
        const ULONGLONG started = ::GetTickCount64();
        const ULONGLONG deadline = started + static_cast<ULONGLONG>(seconds) * 1'000;
        ULONGLONG next_resources = started + 10'000;
        unsigned actions = 0;
        unsigned backward_targets = 0;
        MediaTime previous_target{kSecond};
        const int64_t maximum_ms = (selected_out_ - MediaTime{3 * kSecond / 2}).count() / 10'000;
        do {
            region_ = actions;
            // Mix 10-50 ms bursts with ordinary 100-450 ms clicks. Backward
            // targets deliberately reset the playing clock many times without
            // giving an old high-water mark time to be reached again.
            const MediaTime target{static_cast<int64_t>(200 + Next() %
                static_cast<uint32_t>(maximum_ms - 199)) * 10'000};
            if (target < previous_target) ++backward_targets;
            previous_target = target;
            if (!Seek(target)) return false;
            ++actions;
            const unsigned delay = actions % 4 == 0 ? 10 + Next() % 41
                                                    : 100 + Next() % 351;
            const ULONGLONG next_action = std::min(deadline, ::GetTickCount64() + delay);
            do { Pump(); } while (!failed_ && ::GetTickCount64() < next_action);
            if (failed_) return false;
            if (::GetTickCount64() >= next_resources) {
                Resources sample{};
                if (!resources(sample)) return false;
                next_resources += 10'000;
            }
        } while (::GetTickCount64() < deadline);
        // A final middle seek must converge and then advance without another
        // input, replacement host or delayed second restart of that seek.
        if (!Seek(MediaTime{kSecond}) || !Advancing()) return false;
        const auto settled = preview_.snapshot();
        MediaTime last_position = settled.native_position;
        const ULONGLONG playback_deadline = ::GetTickCount64() + 1'000;
        do {
            Pump();
            if (failed_) return false;
            const auto state = preview_.snapshot();
            if (state.command_pending || !state.native_playing || !state.native_position_valid ||
                state.native_position < last_position)
                return Fail("settled seek keeps advancing without a delayed second restart");
            last_position = state.native_position;
        } while (::GetTickCount64() < playback_deadline);
        if (last_position < settled.native_position + MediaTime{kSecond / 2})
            return Fail("continuous seek final playback advances at least 500 ms");
        continuous_active_ = false;
        if (!Issue(preview_.Pause(), "continuous-final-pause") || !Paused()) return false;
        Resources final{};
        if (!resources(final)) return false;
        // These are deliberately broad bounded-growth guards after a real
        // decoder warmup, not a claim that one short run proves leak freedom.
        if (final.handles > baseline.handles + 64 || final.gdi > baseline.gdi + 8 ||
            final.user > baseline.user + 8 ||
            final.private_bytes > baseline.private_bytes + 64ull * 1024 * 1024)
            return Fail("continuous input keeps helper resources within bounded growth");
        if (actions < seconds * 2 || backward_targets < seconds / 2)
            return Fail("continuous stress executes enough real forward and backward seeks");
        if (!Issue(preview_.PauseAt(selected_out_), "continuous-retain-exclusive-Out") ||
            !Paused()) return false;
        const auto endpoint = preview_.snapshot();
        if (endpoint.position != selected_out_ || !endpoint.native_position_valid ||
            endpoint.native_position >= selected_out_ ||
            endpoint.native_position < selected_out_ - MediaTime{kSecond / 10})
            return Fail("continuous stress retains the final frame before exclusive Out");
        if (!Play() || !Advancing() ||
            !Issue(preview_.Pause(), "continuous-replay-final-pause") || !Paused()) return false;
        if (preview_.snapshot().player_generation != continuous_generation_ ||
            preview_.snapshot().backend_process_id != continuous_process_)
            return Fail("continuous input and endpoint reuse need no helper replacement");
        std::printf("Continuous MFPlay seed=%08X passed: seconds=%u seeks=%u backward=%u "
                    "pending-input=%u max-command-age-ms=%llu process=%u generation=%llu\n",
                    seed_, seconds, actions, backward_targets, pending_input_count_,
                    continuous_max_command_age_, continuous_process_, continuous_generation_);
        return true;
    }

    bool RunReopen(const std::wstring& path, HWND window) {
        selected_out_ = preview_.snapshot().duration;
        for (unsigned cycle = 0; cycle < 6; ++cycle) {
            region_ = cycle;
            const uint64_t obsolete_generation = preview_.snapshot().player_generation;
            if (!Issue(preview_.PlayFrom(MediaTime{6 * kSecond}), "old-generation-play"))
                return false;
            if (cycle % 2) Delay(10);
            if (!Seek(preview_.snapshot().duration - MediaTime{kSecond / 10})) return false;
            // Close with an owned Play/Seek or its notification still queued.
            // Reopen the same object, so old callback lifetime/identity handling
            // is exercised through real native player instances.
            preview_.Close();
            Record("old-generation-closed");
            if (!Issue(preview_.Open(path, window, window, WM_APP + 7), "new-generation-open") ||
                !Until([&] { return preview_.snapshot().state == MediaPreviewState::Ready; },
                       "new generation opens after pending Close")) return false;
            const auto ready = preview_.snapshot();
            if (ready.player_generation <= obsolete_generation)
                return Fail("reopened player has a newer generation");
            selected_out_ = ready.duration;
            Delay(100); // drain real old/new notifications without further intent
            const auto drained = preview_.snapshot();
            if (drained.player_generation != ready.player_generation ||
                drained.state != MediaPreviewState::Ready || drained.play_requested ||
                drained.command_pending || drained.position != MediaTime::zero())
                return Fail("old callbacks cannot alter replacement state");
            if (!Issue(preview_.PlayFrom(selected_in_), "new-generation-play") ||
                !Advancing() || !Issue(preview_.Pause(), "new-generation-pause") || !Paused())
                return false;
        }
        std::printf("MFPlay pending Close/Open generation isolation: 6 cycles passed\n");
        return true;
    }

    bool RunDelayedCompletionRecovery(const std::wstring& path, HWND window) {
        policy_enabled_ = false;
        selected_out_ = preview_.snapshot().duration;
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            if (!Issue(preview_.PauseAt(MediaTime{kSecond}), "recovery-setup") || !Paused())
                return false;
            const auto original = preview_.snapshot();
            SuspendedPreviewHost stalled;
            if (!stalled.Suspend(original.backend_process_id, original.backend_thread_id))
                return Fail("suspend only the owned preview host UI thread");
            const int resize_width = 480 + static_cast<int>(attempt) * 64;
            const int resize_height = 270 + static_cast<int>(attempt) * 36;
            const ULONGLONG resize_started = ::GetTickCount64();
            for (unsigned index = 0; index < 32; ++index) {
                const int delta = static_cast<int>(index % 4);
                if (!::SetWindowPos(window, nullptr, 0, 0,
                                    resize_width + delta * 16, resize_height + delta * 9,
                                    SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER))
                    return Fail("resize the test-owned parent while its native child is stalled");
                preview_.UpdateVideo();
            }
            const ULONGLONG resize_elapsed = ::GetTickCount64() - resize_started;
            if (resize_elapsed > 250)
                return Fail("32 suspended-host resizes return within 250 ms");
            Record("stalled-host-resize-burst");
            if (!Issue(preview_.Seek(MediaTime{6 * kSecond}), "stalled-owned-seek")) return false;
            Delay(150);
            if (!Issue(preview_.Pause(), "stalled-newest-pause")) return false;
            Delay(150);
            if (!Issue(preview_.Seek(MediaTime{2 * kSecond}), "stalled-newest-position"))
                return false;
            Delay(150);
            if (!Issue(preview_.Play(), "stalled-newest-play")) return false;
            if (attempt == 0) {
                if (!Until([&] {
                        const auto state = preview_.snapshot();
                        return state.player_generation > original.player_generation &&
                               state.backend_process_id != 0 &&
                               state.backend_process_id != original.backend_process_id &&
                               stalled.Retired();
                    }, "watchdog retires the stalled host before replacement") || !Advancing())
                    return false;
                const auto recovered = preview_.snapshot();
                if (!recovered.play_requested || recovered.position < MediaTime{2 * kSecond} ||
                    recovered.position >= MediaTime{4 * kSecond} ||
                    recovered.player_generation != original.player_generation + 1)
                    return Fail("host recovery preserves newest position and Play");
                if (!Until([&] {
                        return OwnedVideoHasSize(window, recovered.backend_process_id,
                                                 resize_width + 48, resize_height + 27);
                    }, "replacement child applies the latest coalesced parent dimensions"))
                    return false;
                std::printf("MFPlay watchdog retired owned process=%u and replaced it with process=%u\n",
                            original.backend_process_id, recovered.backend_process_id);
                std::printf("MFPlay stalled-host resize: 32 updates in %llu ms; replacement size %dx%d\n",
                            resize_elapsed, resize_width + 48, resize_height + 27);
            } else {
                // Failed is expected here, so avoid Until(), whose ordinary
                // liveness oracle correctly rejects a Failed preview.
                if (!Wait([&] {
                        return preview_.snapshot().state == MediaPreviewState::Failed &&
                               stalled.Retired();
                    })) return Fail("exhausted recovery explicitly fails and retires the child");
                const auto failed = preview_.snapshot();
                if (failed.state != MediaPreviewState::Failed || failed.command_pending ||
                    failed.native_position_valid || failed.backend_process_id != 0 ||
                    failed.error != HRESULT_FROM_WIN32(ERROR_TIMEOUT))
                    return Fail("failed preview owns no live host or ambiguous native command");
            }
        }
        // Exhaustion is an explicit failure, with the finalized source intact
        // and the same preview object available for a deliberate new Open.
        if (!Issue(preview_.Open(path, window, window, WM_APP + 7), "reopen-after-recovery-limit") ||
            !Until([&] { return preview_.snapshot().state == MediaPreviewState::Ready; },
                   "reopen after explicit recovery failure") || !ConvergeAndReuse()) return false;
        const auto before_close = preview_.snapshot();
        SuspendedPreviewHost stalled_at_close;
        if (!stalled_at_close.Suspend(before_close.backend_process_id, before_close.backend_thread_id))
            return Fail("suspend the owned native host before direct Close");
        const ULONGLONG close_started = ::GetTickCount64();
        preview_.Close();
        const ULONGLONG close_elapsed = ::GetTickCount64() - close_started;
        if (close_elapsed > 1'500 || !stalled_at_close.Retired() ||
            preview_.snapshot().state != MediaPreviewState::Closed ||
            preview_.snapshot().backend_process_id != 0)
            return Fail("Close retires a suspended native host within 1500 ms");
        if (!Issue(preview_.Open(path, window, window, WM_APP + 7), "reopen-after-forced-close") ||
            !Until([&] { return preview_.snapshot().state == MediaPreviewState::Ready; },
                   "reopen after bounded forced Close") || !ConvergeAndReuse()) return false;
        const DWORD healthy_process = preview_.snapshot().backend_process_id;
        for (unsigned index = 0; index < 16; ++index) {
            const int width = index % 2 ? 640 : 400;
            const int height = index % 2 ? 360 : 225;
            if (!::SetWindowPos(window, nullptr, 0, 0, width, height,
                                SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER))
                return Fail("resize the healthy preview parent");
            preview_.UpdateVideo();
        }
        if (!Until([&] {
                return OwnedVideoHasSize(window, healthy_process, 640, 360);
            }, "healthy child applies the newest coalesced resize")) return false;
        if (preview_.snapshot().backend_process_id != healthy_process)
            return Fail("healthy resize converges without replacing the host");
        policy_enabled_ = true;
        std::printf("MFPlay direct Close retired stalled process=%u in %llu ms\n",
                    before_close.backend_process_id, close_elapsed);
        std::printf("MFPlay host stalls: latest-intent recovery, process retirement, bounded retry passed\n");
        std::printf("MFPlay healthy resize: 16 updates converged to 640x360 without replacement\n");
        return true;
    }

    bool RunEndpointRegression() {
        selected_out_ = preview_.snapshot().duration;
        const uint64_t initial_generation = preview_.snapshot().player_generation;
        if (!Issue(preview_.PlayFrom(MediaTime{6 * kSecond}),
                   "endpoint-regression-healthy-play") || !Advancing()) return false;
        if (!Issue(preview_.PauseAt(selected_out_), "endpoint-regression-stationary-Out") ||
            !Paused()) return false;
        const auto endpoint = preview_.snapshot();
        if (endpoint.position != selected_out_ || !endpoint.native_position_valid ||
            endpoint.native_position != MediaTime{359 * kSecond / 30} ||
            endpoint.player_generation != initial_generation)
            return Fail("exclusive logical Out displays the exact last native frame without recovery");
        if (!Issue(preview_.PlayFrom(MediaTime{6 * kSecond}),
                   "endpoint-regression-replay-middle") || !Advancing()) return false;
        // Captured failure: the Pause callback dispatches SetPosition(duration)
        // on an H264/AAC source, which native MFPlay accepts without POSITION_SET.
        // Newer PlayFrom(In), Pause, Play must never remain behind that ownership.
        if (!Issue(preview_.PauseAt(selected_out_), "endpoint-regression-PauseAt"))
            return false;
        Delay(30);
        if (!Issue(preview_.PlayFrom(selected_in_), "endpoint-regression-restart"))
            return false;
        Delay(10);
        if (!Issue(preview_.Pause(), "endpoint-regression-pause")) return false;
        Delay(10);
        if (!Play() || !Advancing() || !ConvergeAndReuse()) return false;
        if (preview_.snapshot().player_generation != initial_generation)
            return Fail("exclusive endpoint restart does not require player recovery");
        std::printf("MFPlay H264/AAC endpoint ownership regression passed without recovery\n");
        return true;
    }

    bool RunFocused(unsigned cycles) {
        const uint64_t initial_generation = preview_.snapshot().player_generation;
        constexpr int64_t offsets_ms[]{500, 250, 100, 0, -100};
        for (unsigned cycle = 0; cycle < cycles; ++cycle) {
            region_ = cycle;
            // Include the UI's default full range and near-natural-end Out:
            // endpoint notification and automatic PauseAt may coincide here.
            const int64_t tail_ms = cycle % 4 == 0 ? 4'000 :
                cycle % 4 == 1 ? 0 : cycle % 4 == 2 ? 10 : 100;
            selected_out_ = preview_.snapshot().duration - MediaTime{tail_ms * 10'000};
            policy_enabled_ = true;
            const MediaTime start = std::min(preview_.snapshot().duration,
                selected_out_ - MediaTime{offsets_ms[cycle % std::size(offsets_ms)] * 10'000});
            if (!Issue(preview_.PauseAt(start), "focus-place") || !Paused() || !Play())
                return false;
            for (unsigned action = 0; action < 16; ++action) {
                Delay(10 + Next() % 91);
                if (!Toggle()) return false;
                if (action % 4 == 3) {
                    if (!Seek(selected_out_ - MediaTime{
                            static_cast<int64_t>(10 + Next() % 91) * 10'000}) || !Play())
                        return false;
                }
            }
            if (!Issue(preview_.PlayFrom(selected_out_ - MediaTime{kSecond / 10}),
                       "focus-final-Out-approach") ||
                !Until([&] { return !preview_.snapshot().play_requested; },
                       "focus endpoint/automatic Out intent") || !Play()) return false;
            Delay(10 + Next() % 41);
            if (!Issue(preview_.Pause(), "focus-restart-reverse")) return false;
            Delay(10 + Next() % 41);
            if (!Play() || !Advancing() || !ConvergeAndReuse()) return false;
            std::printf("Focused MFPlay seed=%08X cycle=%u/%u tail-ms=%lld passed "
                        "automatic-Out=%u pending-input=%u replacements=%llu\n", seed_, cycle + 1,
                        cycles, tail_ms, automatic_out_count_, pending_input_count_,
                        preview_.snapshot().player_generation - initial_generation);
        }
        if (preview_.snapshot().player_generation != initial_generation)
            return Fail("ordinary focused input needs no host replacement");
        return (automatic_out_count_ > 0 && pending_input_count_ > 0) ||
               Fail("focused stress exercises automatic Out and pending user intent");
    }

    bool Run() {
        const uint64_t initial_generation = preview_.snapshot().player_generation;
        constexpr int64_t before_out_ms[]{4'000, 500, 250, 100, 0, -100};
        for (unsigned region = 0; region < std::size(before_out_ms); ++region) {
            region_ = region;
            selected_out_ = MediaTime{8 * kSecond};
            policy_enabled_ = true;
            Record("region-begin");
            if (!Issue(preview_.PauseAt(selected_out_ -
                           MediaTime{before_out_ms[region] * 10'000}),
                       "place-region") || !Paused()) return false;

            // The first action uses the real restart-at-Out planner. During the
            // remaining actions a native callback may complete before or after
            // any opposite intent. Fixed PRNG arithmetic preserves every delay.
            if (!Play()) return false;
            for (unsigned action = 0; action < 18; ++action) {
                Delay(10 + Next() % 291);
                switch (action % 9) {
                    case 0: case 2: case 4: case 6: case 8:
                        if (!Toggle()) return false;
                        break;
                    case 1: case 5:
                        if (!Play()) return false;
                        break;
                    case 3:
                        if (!Seek(selected_out_ - MediaTime{
                                static_cast<int64_t>(20 + Next() % 501) * 10'000}))
                            return false;
                        break;
                    case 7:
                        // Two human-spaced timeline updates can supersede a
                        // queued Seek, followed by Play before it completes.
                        if (!Seek(selected_in_ + MediaTime{kSecond / 2})) return false;
                        Delay(10 + Next() % 31);
                        if (!Seek(selected_in_ + MediaTime{kSecond})) return false;
                        break;
                    default: break;
                }
            }
            // Stop waiting as soon as the production tick submits PauseAt,
            // while native ownership may still be pending. User intent now
            // reverses automatic Out handling before it fully settles.
            if (!Issue(preview_.PlayFrom(selected_out_ - MediaTime{kSecond / 10}),
                       "approach-Out-for-pending-reversal") ||
                !Until([&] { return !preview_.snapshot().play_requested; },
                       "automatic Out intent submitted") || !Play()) return false;
            Delay(10 + Next() % 31);
            if (!Issue(preview_.Pause(), "automatic-Out-restart-pause")) return false;
            Delay(10 + Next() % 31);
            if (!Play() || !Advancing() || !ConvergeAndReuse()) return false;
            std::printf("Human MFPlay stress seed=%08X selected region=%u passed\n",
                        seed_, region_);
        }

        // Exercise actual endpoint events separately, with selected-Out policy
        // disabled so a timer PauseAt cannot stand in for PLAYBACK_ENDED.
        selected_out_ = preview_.snapshot().duration;
        policy_enabled_ = false;
        for (unsigned cycle = 0; cycle < 3; ++cycle) {
            region_ = 6 + cycle;
            Record("natural-end-region");
            if (!Issue(preview_.PlayFrom(selected_out_ - MediaTime{kSecond / 10}),
                       "natural-end-start")) return false;
            Delay(10 + Next() % 71);
            if (!Issue(preview_.Pause(), "natural-end-reverse-pause")) return false;
            Delay(10 + Next() % 71);
            if (!Play()) return false;
            // End may already have been delivered during the human delay, in
            // which case Play validly restarted from In. Request a new endpoint
            // approach before waiting for Ended; never mistake that restart for
            // a transport timeout.
            if (!Issue(preview_.PlayFrom(selected_out_ - MediaTime{kSecond / 10}),
                       "natural-end-final-approach")) return false;
            if (!Until([&] {
                    return preview_.snapshot().state == MediaPreviewState::Ended;
                }, "natural end event")) return false;
            if (!Play()) return false;
            Delay(10 + Next() % 41);
            if (!Issue(preview_.Pause(), "restart-reverse-pause")) return false;
            Delay(10 + Next() % 41);
            if (!Play() || !Advancing() || !ConvergeAndReuse()) return false;
        }
        if (automatic_out_count_ == 0 || pending_input_count_ == 0)
            return Fail("human stress exercises automatic Out and pending user intent");
        if (preview_.snapshot().player_generation != initial_generation)
            return Fail("ordinary human input needs no host replacement");
        std::printf("Human MFPlay stress seed=%08X passed: 6 selected regions, "
                    "3 natural ends, %u automatic Out actions, "
                    "%u input actions while native operation pending\n",
                    seed_, automatic_out_count_, pending_input_count_);
        return true;
    }

private:
    uint32_t Next() {
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        return random_;
    }

    void Record(const char* action) {
        const auto state = preview_.snapshot();
        std::ostringstream line;
        line << "seed=" << seed_ << " region=" << region_
             << " seq=" << ++sequence_ << " ms=" << ::GetTickCount64()
             << " thread=" << ::GetCurrentThreadId() << " action=" << action
             << " in=" << selected_in_.count() << " out=" << selected_out_.count()
             << " logical=" << static_cast<unsigned>(state.state)
             << " desired=" << state.play_requested << " pending=" << state.command_pending
             << " position-pending=" << state.position_pending
             << " position=" << state.position.count()
             << " native-position=" << state.native_position.count()
             << " native-position-valid=" << state.native_position_valid
             << " native-playing=" << state.native_playing
             << " generation=" << state.player_generation
             << " backend-process=" << state.backend_process_id
             << " backend-thread=" << state.backend_thread_id
             << " command=" << state.command_sequence
             << " command-age-ms=" << state.command_age_ms
             << " intent=" << state.intent_sequence
             << " hr=" << static_cast<unsigned>(state.error);
        history_.push_back(line.str());
        if (history_.size() > 160) history_.pop_front();
    }

    bool Fail(const char* label) {
        failed_ = true;
        Record("LIVENESS-FAILURE");
        Check(false, label);
        std::fprintf(stderr, "Human transport failure seed=%08X region=%u: %s\n",
                     seed_, region_, label);
        for (const auto& line : history_) std::fprintf(stderr, "%s\n", line.c_str());
        std::fprintf(stderr, "%ls\n", preview_.diagnostics().c_str());
        return false;
    }

    bool Issue(bool accepted, const char* action) {
        Record(action);
        return !failed_ && (accepted || Fail(action));
    }

    void Input() {
        if (preview_.snapshot().command_pending) ++pending_input_count_;
    }

    bool Apply(const easnap::app::TrimPlaybackDecision& decision) {
        using easnap::app::TrimPlaybackAction;
        Input();
        switch (decision.action) {
            case TrimPlaybackAction::Play:
                return Issue(preview_.Play(), "user-play");
            case TrimPlaybackAction::Pause:
                return Issue(preview_.Pause(), "user-pause");
            case TrimPlaybackAction::SeekThenPlay:
                return Issue(preview_.PlayFrom(decision.position), "user-restart-from-In");
            default: return Fail("unexpected toggle policy action");
        }
    }

    bool Toggle() {
        const auto state = preview_.snapshot();
        return Apply(easnap::app::PlanTrimPlaybackToggle(
            state.play_requested, state.state == MediaPreviewState::Ended,
            state.position, selected_in_, selected_out_));
    }

    bool Play() {
        const auto state = preview_.snapshot();
        // An explicit Play keeps Play as newest intent when already requested.
        if (state.play_requested) {
            Input();
            return Issue(preview_.Play(), "user-reaffirm-play");
        }
        return Toggle();
    }

    bool Seek(MediaTime position) {
        const auto selected = easnap::app::PlanTrimTimelineSeek(
            preview_.snapshot().play_requested, position, selected_in_, selected_out_);
        if (!selected) { Record("timeline-seek-ignored-by-policy"); return true; }
        Input();
        return Issue(preview_.Seek(*selected), "user-seek");
    }

    void Tick() {
        if (!policy_enabled_) return;
        const auto state = preview_.snapshot();
        const auto decision = easnap::app::PlanTrimPlaybackProgress(
            state.play_requested && !state.position_pending,
            state.position, selected_out_);
        if (decision.action == easnap::app::TrimPlaybackAction::PauseAt) {
            ++automatic_out_count_;
            static_cast<void>(Issue(preview_.PauseAt(decision.position),
                                    "automatic-selected-Out"));
        }
    }

    void Pump() {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
            if (message.message == WM_APP + 7) Tick();
        }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= next_tick_) {
            next_tick_ = now + 100;
            Tick();
            Record("timer");
        }
        if (continuous_active_ && !failed_) {
            const auto state = preview_.snapshot();
            continuous_max_command_age_ = std::max(continuous_max_command_age_,
                                                   state.command_age_ms);
            if (state.player_generation != continuous_generation_ ||
                state.backend_process_id != continuous_process_ || state.recovery_requested ||
                !state.play_requested || state.state == MediaPreviewState::Opening ||
                state.state == MediaPreviewState::Closed || state.state == MediaPreviewState::Failed ||
                state.state == MediaPreviewState::Ended)
                static_cast<void>(Fail("healthy continuous seeks preserve Play and the original helper"));
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    }

    void Delay(unsigned milliseconds) {
        const ULONGLONG deadline = ::GetTickCount64() + milliseconds;
        do { Pump(); } while (::GetTickCount64() < deadline);
    }

    template<class Predicate>
    bool Until(Predicate predicate, const char* label) {
        const ULONGLONG deadline = ::GetTickCount64() + 5'000;
        do {
            if (failed_) return false;
            if (preview_.snapshot().state == MediaPreviewState::Failed) return Fail(label);
            if (predicate()) return true;
            Pump();
        } while (::GetTickCount64() < deadline);
        return Fail(label);
    }

    bool Paused() {
        if (!Until([&] {
                const auto state = preview_.snapshot();
                return !state.play_requested && !state.command_pending &&
                       (state.state == MediaPreviewState::Paused ||
                        (state.state == MediaPreviewState::Ended &&
                         state.position == state.duration)) && !state.native_playing;
            }, "final Pause converges coherently")) return false;
        const auto before = preview_.snapshot();
        Delay(100);
        const auto after = preview_.snapshot();
        return (before.position == after.position && !after.native_playing &&
                !after.command_pending && !after.play_requested) ||
               Fail("paused transport remains stationary");
    }

    bool Advancing() {
        if (!Until([&] {
                const auto state = preview_.snapshot();
                return state.play_requested && !state.command_pending &&
                       state.state == MediaPreviewState::Playing && state.native_playing &&
                       state.native_position_valid;
            }, "final Play becomes actively playing")) return false;
        const auto before = preview_.snapshot();
        return Until([&] {
            const auto state = preview_.snapshot();
            return state.play_requested && state.state == MediaPreviewState::Playing &&
                   state.native_playing && state.native_position_valid &&
                   state.position >= before.position + MediaTime{kSecond / 5} &&
                   state.native_position >= before.native_position + MediaTime{kSecond / 5};
        }, "final Play advances logical and native clocks by 200 ms");
    }

    bool ConvergeAndReuse() {
        // Deliberately supersede whatever is pending with Seek/Play; there is
        // ample selected media left, so a stationary Playing state is a defect.
        if (!Seek(selected_in_ + MediaTime{kSecond / 2}) || !Play() || !Advancing())
            return false;
        // Prove a subsequent independent Pause -> Seek -> Play still works.
        if (!Issue(preview_.Pause(), "reuse-pause") || !Paused() ||
            !Seek(selected_in_ + MediaTime{kSecond}) || !Paused() ||
            !Play() || !Advancing()) return false;
        return Issue(preview_.Pause(), "cycle-final-pause") && Paused();
    }

    IMediaPreview& preview_;
    const uint32_t seed_;
    uint32_t random_;
    unsigned region_{0};
    unsigned automatic_out_count_{0};
    unsigned pending_input_count_{0};
    uint64_t sequence_{0};
    ULONGLONG next_tick_{0};
    bool policy_enabled_{true};
    bool failed_{false};
    bool continuous_active_{false};
    uint64_t continuous_generation_{0};
    uint32_t continuous_process_{0};
    uint64_t continuous_max_command_age_{0};
    MediaTime selected_in_{kSecond};
    MediaTime selected_out_{8 * kSecond};
    std::deque<std::string> history_;
};

void ExerciseContinuousSeek(const std::wstring& path, HWND window,
                            int seed_index, unsigned seconds) {
    constexpr uint32_t seeds[]{0x6d2b79f5u, 0x13579bdfu, 0xc001d00du};
    for (int index = 0; index < 3; ++index) {
        if (seed_index >= 0 && index != seed_index) continue;
        auto preview = CreateMfPlayPreview();
        if (!Check(preview && preview->Open(path, window, window, WM_APP + 7),
                   "continuous seek preview opens") ||
            !Check(Wait([&] { return preview->snapshot().state == MediaPreviewState::Ready; }),
                   "continuous seek preview ready")) return;
        HumanTransportStress stress(*preview, seeds[index]);
        const bool passed = stress.RunContinuousSeek(seconds);
        preview->Close();
        if (!passed) return;
    }
}

void ExerciseHumanStress(const std::wstring& path, HWND window,
                         uint32_t focused_seed = 0, unsigned focused_cycles = 0,
                         bool endpoint_regression = false, bool recovery_only = false, int seed_index = -1) {
    constexpr uint32_t seeds[]{0x6d2b79f5u, 0x13579bdfu, 0xc001d00du};
    for (int index=0;index<3;++index) {
        if (seed_index>=0 && index!=seed_index) continue;
        const auto seed=seeds[index];
        auto preview = CreateMfPlayPreview();
        if (!Check(preview && preview->Open(path, window, window, WM_APP + 7),
                   "human stress preview opens")) return;
        if (!Check(Wait([&] { return preview->snapshot().state == MediaPreviewState::Ready; }),
                   "human stress preview ready")) return;
        HumanTransportStress stress(*preview, focused_cycles ? focused_seed : seed);
        const bool passed = recovery_only ? stress.RunDelayedCompletionRecovery(path, window) :
            (!endpoint_regression || stress.RunEndpointRegression()) &&
            ((seed != seeds[0] && !focused_cycles) || stress.RunReopen(path, window)) &&
            ((seed != seeds[0] && !focused_cycles) || stress.RunDelayedCompletionRecovery(path, window)) &&
            (focused_cycles ? stress.RunFocused(focused_cycles) : stress.Run());
        preview->Close();
        if (!passed || focused_cycles || recovery_only) return;
    }
}

void ExerciseGenuineFailure(const std::filesystem::path& path, HWND window) {
    { std::ofstream file(path, std::ios::binary); file << "not a media container"; }
    PreviewVideoEndpoint endpoint{MediaTime{kSecond}, MediaTime{2 * kSecond}, 7};
    Check(FAILED(ReadPreviewVideoEndpoint(path.wstring(), MediaTime{12 * kSecond}, endpoint)),
          "invalid media fails endpoint discovery explicitly");
    Check(endpoint.last_frame_position == MediaTime::zero() &&
              endpoint.video_end == MediaTime::zero() && endpoint.samples_examined == 0,
          "failed endpoint discovery never returns guessed frame metadata");
    auto preview = CreateMfPlayPreview();
    Check(preview != nullptr, "failure preview creation");
    if (!preview) return;
    static_cast<void>(preview->Open(path.wstring(), window, window, WM_APP + 7));
    Check(Wait([&] { return preview->snapshot().state == MediaPreviewState::Failed; }),
          "genuine MFPlay error surfaced");
    const auto failure = preview->snapshot();
    Check(FAILED(failure.error), "genuine HRESULT retained");
    Check(!preview->Play() && !preview->Pause(), "failed transport rejects operations");
    Check(preview->snapshot().state == MediaPreviewState::Failed,
          "later operations cannot resurrect Failed");
    Check(std::filesystem::exists(path), "failed source preserved");
    preview->Close();
    std::filesystem::remove(path);
}

// Optional child-only debugger capture: records production OutputDebugString
// diagnostics without changing the logger or observing any other process.
int TraceChild(int argc, wchar_t** argv) {
    wchar_t executable[MAX_PATH]{};
    if (!::GetModuleFileNameW(nullptr, executable, MAX_PATH)) return 1;
    std::wstring command = L"\"" + std::wstring(executable) + L"\"";
    for (int index = 2; index < argc; ++index) {
        // Forward only supported fixed options to the debugger's own child.
        if (std::wstring(argv[index]) == L"--stress" ||
            std::wstring(argv[index]) == L"--native" ||
            std::wstring(argv[index]) == L"--focus" ||
            std::wstring(argv[index]) == L"--focus-audio" ||
            std::wstring(argv[index]) == L"--recovery-audio" ||
            std::wstring(argv[index]).find_first_not_of(L"0123456789abcdefABCDEFxX") ==
                std::wstring::npos)
            command += L" " + std::wstring(argv[index]);
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!::CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                          DEBUG_ONLY_THIS_PROCESS, nullptr, nullptr,
                          &startup, &process)) return 1;
    DWORD result = 1;
    bool running = true;
    while (running) {
        DEBUG_EVENT event{};
        if (!::WaitForDebugEventEx(&event, INFINITE)) break;
        DWORD disposition = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (event.u.CreateProcessInfo.hFile)
                    ::CloseHandle(event.u.CreateProcessInfo.hFile);
                break;
            case LOAD_DLL_DEBUG_EVENT:
                if (event.u.LoadDll.hFile) ::CloseHandle(event.u.LoadDll.hFile);
                break;
            case OUTPUT_DEBUG_STRING_EVENT: {
                const auto& output = event.u.DebugString;
                if (output.fUnicode) {
                    std::vector<wchar_t> text(output.nDebugStringLength + 1, 0);
                    if (::ReadProcessMemory(process.hProcess, output.lpDebugStringData,
                                            text.data(), output.nDebugStringLength *
                                            sizeof(wchar_t), nullptr) &&
                        std::wstring(text.data()).find(L"[Preview]") != std::wstring::npos)
                        std::wprintf(L"%ls", text.data());
                }
                break;
            }
            case EXCEPTION_DEBUG_EVENT:
                if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                    disposition = DBG_EXCEPTION_NOT_HANDLED;
                break;
            case EXIT_PROCESS_DEBUG_EVENT:
                result = event.u.ExitProcess.dwExitCode;
                running = false;
                break;
            default: break;
        }
        ::ContinueDebugEvent(event.dwProcessId, event.dwThreadId, disposition);
    }
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return static_cast<int>(result);
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    if (argc > 1 && std::wstring(argv[1]) == L"--trace") return TraceChild(argc, argv);
    // Screen-capture regions are physical pixels, as in the production host.
    // A DPI-virtualized test parent otherwise measures a different rectangle
    // from its per-monitor-aware helper and the desktop capture provider.
    easnap::platform::EnableDpiAwareness();
    const bool startup_only = argc > 1 && std::wstring(argv[1]) == L"--startup";
    const bool transport_only = argc > 1 && std::wstring(argv[1]) == L"--transport";
    const bool stress_seed = argc > 1 && std::wstring(argv[1]) == L"--stress-seed";
    const bool continuous_seek = argc > 1 && std::wstring(argv[1]) == L"--continuous-seek";
    unsigned continuous_seconds = 30;
    int seed_index=-1;
    if (stress_seed) {
        if (argc!=3 || std::wcslen(argv[2])!=1 || argv[2][0]<L'0' || argv[2][0]>L'2') return 2;
        seed_index=argv[2][0]-L'0';
    }
    if (continuous_seek) {
        if (argc > 4) return 2;
        if (argc > 2) {
            if (std::wcslen(argv[2]) != 1 || argv[2][0] < L'0' || argv[2][0] > L'2') return 2;
            seed_index = argv[2][0] - L'0';
        }
        if (argc > 3) {
            wchar_t* end = nullptr;
            continuous_seconds = static_cast<unsigned>(std::wcstoul(argv[3], &end, 10));
            if (continuous_seconds < 15 || continuous_seconds > 300 || !end || *end) return 2;
        }
    }
    const bool recovery_only = argc > 1 && std::wstring(argv[1]) == L"--recovery-audio";
    const bool audio_fixture = continuous_seek || recovery_only ||
        (argc > 1 && std::wstring(argv[1]) == L"--focus-audio");
    const bool focused = (!continuous_seek && audio_fixture) ||
        (argc > 1 && std::wstring(argv[1]) == L"--focus");
    uint32_t focused_seed = 0x6d2b79f5u;
    unsigned focused_cycles = 30;
    if (focused && argc > 2) {
        wchar_t* end = nullptr;
        focused_seed = static_cast<uint32_t>(std::wcstoul(argv[2], &end, 0));
        if (!focused_seed || !end || *end) return 2;
    }
    if (focused && argc > 3) {
        wchar_t* end = nullptr;
        focused_cycles = static_cast<unsigned>(std::wcstoul(argv[3], &end, 10));
        if (!focused_cycles || focused_cycles > 1'000 || !end || *end) return 2;
    }
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return 1;
    if (FAILED(::MFStartup(MF_VERSION))) { ::CoUninitialize(); return 1; }
    const auto directory = std::filesystem::temp_directory_path() /
        (L"easnap-preview-test-" + std::to_wstring(::GetCurrentProcessId()) +
         L"-" + std::to_wstring(::GetTickCount64()));
    std::filesystem::create_directory(directory);
    const auto path = directory / L"preview.mp4";
    const RECT placement=easnap::test::OnTestMonitor({0,0,320,192});
    HWND window = startup_only ? easnap::test::DesktopTestWindow::Create() :
        ::CreateWindowExW(0, L"STATIC", L"Preview test", WS_POPUP|WS_CLIPCHILDREN,
                                    placement.left, placement.top, 320, 192, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    std::printf("Preview test DPI: parent=%u awareness=%d\n",::GetDpiForWindow(window),
        static_cast<int>(::GetAwarenessFromDpiAwarenessContext(::GetWindowDpiAwarenessContext(window))));
    if (startup_only && Check(window != nullptr,"startup video window")) {
        ExerciseInitialFrame(directory,window);
        if (!failures) ExerciseRetainedEndpoints(directory,window);
    } else if (Check(window != nullptr &&
                  (audio_fixture
                       ? easnap::test::CreatePreviewSource(path.wstring(), true,
                                                           continuous_seek ? 8u : 12u)
                       : MakeSource(path.wstring())),
              continuous_seek ? "8 second 640x360 H264/AAC fixture" :
              audio_fixture ? "12 second 640x360 H264/AAC fixture"
                            : "12 second fixture")) {
        if (continuous_seek) {
            ExerciseContinuousSeek(path.wstring(), window, seed_index, continuous_seconds);
        } else if (argc <= 1 || std::wstring(argv[1]) != L"--native") {
            if (!stress_seed) ProbeNative(path.wstring(), window, audio_fixture);
            // Presented startup pixels have their own desktop acceptance test.
            // Keep transport stress independent of foreground/occlusion state.
            if (!focused && !stress_seed && (argc <= 1 || std::wstring(argv[1]) != L"--stress"))
                ExercisePreview(path.wstring(), window);
            if (!transport_only) ExerciseHumanStress(path.wstring(), window,
                                focused ? focused_seed : 0, focused ? focused_cycles : 0,
                                audio_fixture, recovery_only, seed_index);
            if (!stress_seed) ExerciseGenuineFailure(directory / L"invalid.mp4", window);
        } else {
            ProbeNative(path.wstring(), window, audio_fixture);
        }
    }
    if (window) ::DestroyWindow(window);
    std::filesystem::remove(path);
    std::filesystem::remove(directory);
    easnap::platform::GraphicsEnvironment::Get().Shutdown();
    ::MFShutdown();
    ::CoUninitialize();
    std::printf("MFPlay preview: %d failures\n", failures);
    return failures ? 1 : 0;
}
