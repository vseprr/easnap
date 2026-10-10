// Real production HWNDs, annotation compositor and MFPlay preview. Generated
// media/pixels are test inputs only; no capture, export or UI logic is duplicated.
#include "platform/platform_utils.h"
#include <UIAutomation.h>
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <psapi.h>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>
#include <thread>
#include "app/app_settings.h"
#include "editor/screenshot_editor.h"
#include "ui/native_controls.h"
#include "ui/overlay.h"
#include "ui/recording_controls.h"
#include "ui/trim_window.h"
#include "preview_test_fixture.h"
#include "presented_desktop_capture.h"
#include "native_test_monitor.h"

using namespace easnap;
namespace {
int failures = 0;
#define CHECK(value) do { if (!(value)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #value); } } while (false)
void Pump(unsigned milliseconds) {
    const auto until = ::GetTickCount64() + milliseconds;
    do {
        MSG message{};
        while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&message); ::DispatchMessageW(&message);
        }
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
    } while (::GetTickCount64() < until);
}
bool WaitFor(const std::function<bool()>& ready,unsigned timeout=2000) {
    const auto until=::GetTickCount64()+timeout;
    // A readiness probe may consume a newly presented frame. Do not probe
    // again after success and mistake the now-empty queue for a failure.
    for (;;) {
        if (ready()) return true;
        if (::GetTickCount64()>=until) return false;
        Pump(10);
    }
}
RECT Bounds(HWND window) { RECT r{}; ::GetWindowRect(window, &r); return r; }
bool Same(const RECT& a, const RECT& b) { return ::EqualRect(&a, &b) != FALSE; }
void Click(HWND parent, int id) { CHECK(::GetDlgItem(parent, id) != nullptr); ::SendMessageW(::GetDlgItem(parent, id), BM_CLICK, 0, 0); }
void Appearance(app::AppearanceTheme value) { ui::theme::ApplyPreference(value); ui::theme::NotifyWindows(); }
bool AutomationClient(const std::function<bool()>& operation) {
    std::atomic<bool> done=false; bool valid=false;
    std::thread client([&] {
        ::CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        valid=operation(); ::CoUninitialize(); done=true;
    });
    while (!done) { Pump(10); }
    client.join(); return valid;
}
std::filesystem::path evidence;
void RecordingOwnerLifetime() {
    const RECT region=test::OnTestMonitor({100,100,740,460});
    ui::RecordingControls recording({});
    DWORD warm_gdi=0,warm_user=0;
    for(int cycle=0;cycle<12;++cycle) {
        HWND owner=::CreateWindowExW(0,L"STATIC",L"Recording lifetime owner",WS_POPUP,
            region.left,region.top,1,1,nullptr,nullptr,::GetModuleHandleW(nullptr),nullptr);
        CHECK(owner && recording.ShowConfiguring(region,{},true,owner));
        const HWND panel=recording.hwnd();
        CHECK(panel && ::GetWindow(panel,GW_OWNER)==owner);
        CHECK(::DestroyWindow(owner));
        CHECK(!::IsWindow(panel));
        const bool retired=recording.hwnd()==nullptr && !recording.visible();
        CHECK(retired);
        if(!retired) { recording.Destroy();return; }
        // A destroyed owner must not leave stale child handles reachable by
        // later state/theme/lifetime operations, or prevent reopening.
        recording.SetError(L"Owner retired");recording.RefreshLayout();recording.Hide();
        CHECK(recording.ShowConfiguring(region,{}));
        CHECK(recording.hwnd() && ::IsWindow(recording.hwnd()) && recording.visible());
        recording.Destroy();recording.Destroy();
        CHECK(recording.hwnd()==nullptr && !recording.visible());
        const DWORD gdi=::GetGuiResources(::GetCurrentProcess(),GR_GDIOBJECTS);
        const DWORD user=::GetGuiResources(::GetCurrentProcess(),GR_USEROBJECTS);
        if(!cycle) { warm_gdi=gdi;warm_user=user; }
        else { CHECK(gdi<=warm_gdi && user<=warm_user); }
        std::printf("Owner lifetime cycle %d: GDI=%lu USER=%lu\n",cycle+1,gdi,user);
    }
}
void MaterialRasterEvidence() {
    uint64_t hash=14695981039346656037ull;
    const auto start=::GetTickCount64();
    unsigned cases=0;
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(theme);
        for(float scale:{1.0f,1.25f,1.5f}) for(int width:{47,96,213,1001})
        for(int height:{14,42}) for(float radius:{7.0f,20.0f})
        for(float depth:{-1.0f,-.35f,0.0f,1.25f}) for(bool drop:{false,true}) {
            const RECT bounds{20,20,20+width,20+height};
            platform::BgraImage image;
            image.width=width+40;image.height=height+40;
            image.pixels.assign(static_cast<size_t>(image.width)*image.height,0xff123456);
            {
                platform::GdiImageSurface surface(image);CHECK(surface.valid());
                ui::theme::DrawMaterial(surface.dc(),bounds,radius,scale,ui::theme::Current().raised,depth,drop);
                ::GdiFlush();
            }
            for(auto pixel:image.pixels) { hash^=pixel&0xffffff;hash*=1099511628211ull; }
            ++cases;
        }
    }
    std::printf("Material rasters: cases=%u hash=%016llx elapsed_ms=%llu\n",cases,
        static_cast<unsigned long long>(hash),static_cast<unsigned long long>(::GetTickCount64()-start));
    // Recorded from the unsliced production renderer before optimization.
    CHECK(cases==768 && hash==0x527290a4517b5712ull);
}
void Shot(HWND window, const wchar_t* name, bool print_client = false) {
    if (evidence.empty()) { return; }
    RECT r{}; ::GetClientRect(window, &r);
    platform::BgraImage image; image.width = r.right; image.height = r.bottom;
    image.pixels.resize(static_cast<size_t>(image.width) * image.height);
    {
        platform::GdiImageSurface surface(image);
        CHECK(surface.valid());
        if(print_client || (::GetWindowLongPtrW(window,GWL_STYLE)&WS_CHILD)) { ::SendMessageW(window,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(surface.dc()),PRF_CLIENT); }
        else { CHECK(::PrintWindow(window, surface.dc(), PW_CLIENTONLY) != FALSE); }
        ::GdiFlush();
    }
    for (auto& pixel : image.pixels) { pixel |= 0xff000000; }
    CHECK(platform::SaveImage(image, (evidence / name).wstring()));
}
double Luminance(COLORREF color) {
    const auto channel = [](BYTE value) { const double c = value / 255.0; return c <= .04045 ? c / 12.92 : std::pow((c + .055) / 1.055, 2.4); };
    return .2126 * channel(GetRValue(color)) + .7152 * channel(GetGValue(color)) + .0722 * channel(GetBValue(color));
}
double Contrast(COLORREF a, COLORREF b) { const double x=Luminance(a), y=Luminance(b); return (std::max(x,y)+.05)/(std::min(x,y)+.05); }
void PaletteContrast() {
    for (auto value : {app::AppearanceTheme::Light, app::AppearanceTheme::Dark}) {
        const auto p = ui::theme::NormalPalette(value);
        for (auto background : {p.panel, p.raised, p.recessed, p.hover, p.pressed, p.selected}) {
            CHECK(Contrast(p.text, background) >= 4.5);
            CHECK(Contrast(p.muted_text, background) >= 4.5);
            CHECK(Contrast(p.disabled, background) >= 4.5);
            CHECK(Contrast(p.control_border, background) >= 3);
            CHECK(Contrast(p.focus, background) >= 3);
            CHECK(Contrast(p.success, background) >= 4.5);
        }
        CHECK(Contrast(p.on_accent, p.accent) >= 4.5);
        CHECK(Contrast(p.on_error, p.error) >= 4.5);
        CHECK(Contrast(p.control_border, p.raised) >= 3);
        CHECK(Contrast(p.focus, p.panel) >= 3);
        CHECK(Contrast(p.success, p.panel) >= 4.5);
        CHECK(Contrast(p.error, p.panel) >= 4.5);
        CHECK(Contrast(p.playhead, p.panel) >= 3);
        CHECK(Contrast(p.panel, p.timeline_selection) >= 3); // Playhead outline against selected track.
        std::printf("Theme %d contrast: text %.2f secondary %.2f primary %.2f boundary %.2f\n",
            static_cast<int>(value), Contrast(p.text,p.panel), Contrast(p.muted_text,p.panel), Contrast(p.on_accent,p.accent), Contrast(p.control_border,p.raised));
    }
}
void RecordingAndEditor() {
    int starts=0, cancels=0, pauses=0, resumes=0, stops=0, prepares=0, status_ticks=0;
    ui::RecordingControls::Callbacks callbacks;
    callbacks.on_start = [&](app::RecordingConfiguration) { ++starts; };
    callbacks.on_cancel = [&] { ++cancels; };
    callbacks.on_pause = [&] { ++pauses; };
    callbacks.on_resume = [&] { ++resumes; };
    callbacks.on_stop = [&] { ++stops; };
    callbacks.on_status_tick = [&] { ++status_ticks; };
    callbacks.on_appearance = Appearance;
    ui::RecordingControls recording(std::move(callbacks));
    app::RecordingConfiguration configuration;
    configuration.mp4_audio = media::AudioSource::Both;
    configuration.include_cursor = true;
    CHECK(recording.ShowConfiguring(test::OnTestMonitor({100,100,740,460}), configuration));
    const auto panel=recording.hwnd();
    CHECK(panel != nullptr);
    const auto hit=[&](int x,int y) {
        const float scale=static_cast<float>(::GetDpiForWindow(panel))/96.0f;
        POINT point{ui::theme::Scaled(static_cast<float>(x),scale),ui::theme::Scaled(static_cast<float>(y),scale)};
        ::ClientToScreen(panel,&point);
        return ::SendMessageW(panel,WM_NCHITTEST,0,MAKELPARAM(point.x,point.y));
    };
    const auto control_hit=[&](int id) {
        const auto box=Bounds(::GetDlgItem(panel,id));
        return ::SendMessageW(panel,WM_NCHITTEST,0,MAKELPARAM((box.left+box.right)/2,(box.top+box.bottom)/2));
    };
    CHECK(hit(200,4)==HTCAPTION && hit(200,148)==HTCAPTION);
    CHECK(control_hit(100)==HTCAPTION && control_hit(IDOK)==HTCLIENT && control_hit(105)==HTCLIENT);
    const auto region=Bounds(panel);
    const auto focus=::GetFocus();
    DWORD affinity=0; CHECK(::GetWindowDisplayAffinity(panel,&affinity) && affinity==0x11);
    // Menu commands, including Windows accessibility activation, go through the
    // same native owner callback rather than a second settings implementation.
    ::SendMessageW(panel,WM_COMMAND,ui::kAppearanceDark,0);
    CHECK(ui::theme::Preference()==app::AppearanceTheme::Dark && starts==0 && cancels==0);
    ::SendMessageW(panel,WM_COMMAND,ui::kAppearanceLight,0);
    CHECK(ui::theme::Preference()==app::AppearanceTheme::Light && starts==0 && cancels==0);
    CHECK(AutomationClient([&] {
        platform::ComPtr<IUIAutomation> automation;
        if (FAILED(::CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))) { return false; }
        for (int id : {111,112,113}) {
            platform::ComPtr<IUIAutomationElement> element;
            if (FAILED(automation->ElementFromHandle(::GetDlgItem(panel,id),&element))) { return false; }
            platform::ComPtr<IUIAutomationTogglePattern> pattern;
            if (FAILED(element->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(&pattern)))) { return false; }
            ToggleState state{};
            if (FAILED(pattern->get_CurrentToggleState(&state)) || state!=ToggleState_On) { return false; }
        }
        platform::ComPtr<IUIAutomationElement> element;
        if (FAILED(automation->ElementFromHandle(::GetDlgItem(panel,IDOK),&element))) { return false; }
        platform::ComPtr<IUIAutomationInvokePattern> invoke;
        return SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,IID_PPV_ARGS(&invoke)));
    }));
    for (int i=0;i!=40;++i) {
        Appearance(i%2 ? app::AppearanceTheme::Light : app::AppearanceTheme::Dark);
        CHECK(recording.hwnd()==panel && Same(region,Bounds(panel)) && ::GetFocus()==focus);
        CHECK(recording.configuration().mp4_audio==media::AudioSource::Both);
        CHECK(recording.configuration().include_cursor);
        CHECK(starts==0 && cancels==0 && pauses==0 && resumes==0 && stops==0);
    }
    Shot(panel,L"record-config-light.png");
    Appearance(app::AppearanceTheme::Dark); Shot(panel,L"record-config-dark.png");
    Click(panel,106); CHECK(recording.configuration().format==media::RecordingFormat::Gif);
    Shot(panel,L"record-gif-dark.png");
    Appearance(app::AppearanceTheme::Light); Shot(panel,L"record-gif-light.png");
    CHECK(recording.configuration().mp4_audio==media::AudioSource::Both);
    Click(panel,105);
    recording.SetError(L"Recording could not start. Check your audio devices and try again.");
    Shot(panel,L"record-retry-light.png"); recording.SetError(L"");
    {
        ui::PopupScope scope(::GetDlgItem(panel,115));
        CHECK(ui::PopupActive());
        CHECK(ui::ConsumePopupInput(WM_KEYDOWN,VK_ESCAPE));
        CHECK(ui::ConsumePopupInput(WM_LBUTTONUP));
        Appearance(app::AppearanceTheme::Dark);
        CHECK(starts==0 && cancels==0);
    }
    CHECK(!ui::PopupActive());
    recording.SetSessionState(app::SessionState::RecordingActive);
    recording.SetElapsed(65ull*10'000'000);
    Shot(panel,L"recording-active-dark.png");
    Click(panel,120); CHECK(pauses==1);
    // Theme repaint must not turn a live panel into configuration or reset time.
    Appearance(app::AppearanceTheme::Light); Shot(panel,L"recording-active-light.png");
    recording.SetSessionState(app::SessionState::RecordingPaused);
    CHECK(control_hit(101)==HTCAPTION && control_hit(120)==HTCLIENT && control_hit(121)==HTCLIENT);
    {
        const HWND resume=::GetDlgItem(panel,120);
        RECT box{}; ::GetClientRect(resume,&box);
        const float scale=static_cast<float>(::GetDpiForWindow(resume))/96.0f;
        CHECK(box.right==ui::theme::Scaled(40,scale) && box.bottom==ui::theme::Scaled(40,scale));
        RECT client{}; ::GetClientRect(panel,&client);
        CHECK(client.right==ui::theme::Scaled(310,scale));
        const RECT pause=Bounds(resume),elapsed=Bounds(::GetDlgItem(panel,101)),stop=Bounds(::GetDlgItem(panel,121));
        CHECK(std::abs((elapsed.left+elapsed.right)-(pause.right+stop.left))<=1);
        CHECK(pause.right<elapsed.left && elapsed.right<stop.left);
        // The hidden visual label remains available to native accessibility.
        wchar_t name[16]{}; ::GetWindowTextW(resume,name,16); CHECK(std::wstring(name)==L"Resume");
        CHECK(::SendMessageW(resume,BM_GETCHECK,0,0)==BST_CHECKED);
    }
    Click(panel,120); CHECK(resumes==1);
    Shot(panel,L"recording-paused-light.png");
    Appearance(app::AppearanceTheme::Dark); Shot(panel,L"recording-paused-dark.png");
    recording.SetSessionState(app::SessionState::RecordingFinalizing);
    const int ticks_before_finalizing = status_ticks;
    const ULONGLONG completion_poll_deadline = ::GetTickCount64() + 1000;
    while (status_ticks == ticks_before_finalizing &&
           ::GetTickCount64() < completion_poll_deadline) { Pump(10); }
    CHECK(status_ticks > ticks_before_finalizing);
    Shot(panel,L"recording-finalizing-dark.png");
    recording.Hide();
    const int ticks_after_hide = status_ticks;
    Pump(300);
    CHECK(status_ticks == ticks_after_hide);
    recording.Destroy();

    app::AppSettings settings;
    editor::ScreenshotEditor::Callbacks editor_callbacks;
    editor_callbacks.on_prepare_image = [&] { ++prepares; return false; };
    editor::ScreenshotEditor editor(std::move(editor_callbacks));
    CHECK(editor.Open(test::OnTestMonitor({100,100,420,300}), &settings, nullptr));
    Appearance(app::AppearanceTheme::Light); Shot(editor.hwnd(),L"editor-provisional-light.png");
    Appearance(app::AppearanceTheme::Dark); Shot(editor.hwnd(),L"editor-provisional-dark.png");
    CHECK(!editor.has_image() && prepares==0);
    platform::BgraImage input; input.width=320;input.height=200;
    input.pixels.resize(320u*200u);
    for(size_t i=0;i!=input.pixels.size();++i) { input.pixels[i]=0xff000000u|static_cast<uint32_t>(i*317u); }
    CHECK(editor.SetImage(std::move(input)));
    Click(editor.hwnd(),2003);
    editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({130,130})); editor.ImagePointer(WM_MOUSEMOVE,test::OnTestPoint({280,220})); editor.ImagePointer(WM_LBUTTONUP,test::OnTestPoint({280,220}));
    const auto before=editor.Composite(); const auto revision=editor.revision();
    CHECK(revision>0);
    const auto frame=editor.region(); const auto island=Bounds(editor.hwnd());
    // Warm the native font/theme caches before checking steady-state lifetime.
    for (int i=0;i!=20;++i) {
        Appearance(i%2 ? app::AppearanceTheme::Light : app::AppearanceTheme::Dark);
        ::RedrawWindow(editor.hwnd(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
    }
    const DWORD gdi_before=::GetGuiResources(::GetCurrentProcess(),GR_GDIOBJECTS);
    for(int i=0;i!=100;++i) {
        Appearance(i%2 ? app::AppearanceTheme::Light : app::AppearanceTheme::Dark);
        ::RedrawWindow(editor.hwnd(),nullptr,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_ALLCHILDREN);
        CHECK(editor.revision()==revision && Same(editor.region(),frame) && Same(Bounds(editor.hwnd()),island));
    }
    const DWORD gdi_after=::GetGuiResources(::GetCurrentProcess(),GR_GDIOBJECTS);
    CHECK(gdi_after<=gdi_before+2);
    std::printf("100 theme switches: GDI objects %lu -> %lu; compositor pixels identical\n",gdi_before,gdi_after);
    CHECK(editor.Composite().pixels==before.pixels);
    Shot(editor.hwnd(),L"editor-rectangle-light.png");
    Appearance(app::AppearanceTheme::Dark);Shot(editor.hwnd(),L"editor-rectangle-dark.png");
    Click(editor.hwnd(),2006);Shot(editor.hwnd(),L"editor-mosaic-dark.png");
    Click(editor.hwnd(),2401); CHECK(editor.revision()>revision);
    Click(editor.hwnd(),2402); CHECK(editor.Composite().pixels==before.pixels);
    editor.Destroy();
}

void NativeMaterialChecks() {
    editor::ScreenshotEditor editor({}); CHECK(editor.Open(test::OnTestMonitor({50,50,650,450}),nullptr,nullptr));
    for(auto preference:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(preference); HWND button=::GetDlgItem(editor.hwnd(),2407);
        for(int state=0;state<5;++state) {
            ::EnableWindow(button,state!=4); ::SendMessageW(button,BM_SETSTATE,state==2,0);
            if(state==1) { ::SendMessageW(button,WM_MOUSEMOVE,0,0); }
            if(state==3) { ::SetFocus(button); }
            Pump(180);
            const auto name=(preference==app::AppearanceTheme::Light ? L"control-light-":L"control-dark-")+std::to_wstring(state)+L".png";
            Shot(button,name.c_str());
        }
        ::EnableWindow(button,TRUE);
        HMENU menu=ui::CreateAppearanceMenu();
        {
            ui::MenuThemeScope themed(editor.hwnd(),menu);
            MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_DATA; CHECK(::GetMenuItemInfoW(menu,0,TRUE,&item));
            CHECK((item.fType&MFT_OWNERDRAW)!=0);
            MEASUREITEMSTRUCT measure{};measure.CtlType=ODT_MENU;measure.itemData=item.dwItemData;
            CHECK(::SendMessageW(editor.hwnd(),WM_MEASUREITEM,0,reinterpret_cast<LPARAM>(&measure))==TRUE);
            CHECK(measure.itemWidth>100 && measure.itemHeight>=30);
        }
        MENUITEMINFOW item{sizeof(item)};item.fMask=MIIM_FTYPE|MIIM_DATA; CHECK(::GetMenuItemInfoW(menu,0,TRUE,&item));
        CHECK((item.fType&MFT_OWNERDRAW)==0); ::DestroyMenu(menu);
        ::SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR id,DWORD) {
            ::KillTimer(nullptr,id);
            ::EnumThreadWindows(::GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
                wchar_t cls[32]{}; ::GetClassNameW(window,cls,32);
                if(::lstrcmpW(cls,L"#32770")==0) {
                    Shot(window,ui::theme::Preference()==app::AppearanceTheme::Light ? L"dialog-light.png":L"dialog-dark.png");
                    ::EndDialog(window,IDOK); return FALSE;
                }
                return TRUE;
            },0);
        });
        CHECK(ui::ShowMessage(editor.hwnd(),L"Your screenshot is ready.\nNative keyboard and accessibility behavior is preserved.",L"Easnap",MB_OK)==IDOK);
        ::SetTimer(nullptr,0,100,[](HWND,UINT,UINT_PTR id,DWORD) {
            ::KillTimer(nullptr,id);
            ::EnumThreadWindows(::GetCurrentThreadId(),[](HWND window,LPARAM)->BOOL {
                wchar_t cls[32]{}; ::GetClassNameW(window,cls,32);
                if(::lstrcmpW(cls,L"#32770")!=0) { return TRUE; }
                RECT client{}; ::GetClientRect(window,&client); const auto outer=Bounds(window);
                CHECK(client.right==outer.right-outer.left && client.bottom==outer.bottom-outer.top);
                CHECK(::GetDlgCtrlID(::GetFocus())==IDNO);
                CHECK(!::IsWindowEnabled(::GetWindow(window,GW_OWNER)));
                CHECK(::FindWindowExW(window,nullptr,L"EasnapWindowChrome",nullptr));
                Shot(window,ui::theme::Preference()==app::AppearanceTheme::Light ? L"discard-light.png":L"discard-dark.png");
                ::SendMessageW(window,WM_CLOSE,0,0); return FALSE;
            },0);
        });
        CHECK(ui::ShowMessage(editor.hwnd(),L"Discard this recording?\n\nThe unsaved recording and its temporary trim data will be permanently deleted.",L"Discard recording",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)==IDNO);
    }
    editor.Destroy();
}

platform::BgraImage RenderEditor(editor::ScreenshotEditor& editor) {
    platform::BgraImage image; const auto region=editor.region(); image.Reset(region.right-region.left,region.bottom-region.top);
    { platform::GdiImageSurface surface(image); CHECK(surface.valid()); editor.RenderImage(surface.dc(),region); }
    for(auto& pixel:image.pixels) { pixel|=0xff000000; } return image;
}
platform::BgraImage RenderControl(HWND control) {
    RECT r{}; ::GetClientRect(control,&r); platform::BgraImage image; image.Reset(r.right,r.bottom);
    { platform::GdiImageSurface surface(image); CHECK(surface.valid()); ::SendMessageW(control,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(surface.dc()),PRF_CLIENT); ::GdiFlush(); }
    for(auto& pixel:image.pixels) { pixel|=0xff000000; } return image;
}
platform::BgraImage PresentedControl(HWND control) {
    RECT r{}; ::GetClientRect(control,&r); platform::BgraImage image; image.Reset(r.right,r.bottom);
    HDC dc=::GetDC(control);
    CHECK(dc!=nullptr && r.right>0 && r.bottom>0);
    { platform::GdiImageSurface surface(image); CHECK(surface.valid()); CHECK(::BitBlt(surface.dc(),0,0,r.right,r.bottom,dc,0,0,SRCCOPY)); ::GdiFlush(); }
    ::ReleaseDC(control,dc);
    for(auto& pixel:image.pixels) { pixel|=0xff000000; } return image;
}
void SurfaceStability() {
    using namespace editor;
    const auto monitor=test::SelectedTestMonitor() ? *test::SelectedTestMonitor():platform::EnumerateMonitors().front();
    ScreenshotEditor editor({}); CHECK(editor.Open(monitor.bounds,nullptr,nullptr));
    RECT main=Bounds(editor.hwnd());
    CHECK(std::abs((main.top+main.bottom)-(monitor.work_area.top+monitor.work_area.bottom))<200);
    editor.Destroy();
    const RECT region=test::OnTestMonitor({40,40,940,600}); CHECK(editor.Open(region,nullptr,nullptr));
    platform::BgraImage source; source.Reset(900,560); std::fill(source.pixels.begin(),source.pixels.end(),0xff53718b);
    CHECK(editor.SetImage(source));
    const auto click=[&](POINT p) { p=test::OnTestPoint(p); editor.ImagePointer(WM_LBUTTONDOWN,p); editor.ImagePointer(WM_LBUTTONUP,p); };
    const auto draw=[&](POINT a,POINT b) { a=test::OnTestPoint(a);b=test::OnTestPoint(b); editor.ImagePointer(WM_LBUTTONDOWN,a); editor.ImagePointer(WM_MOUSEMOVE,b); editor.ImagePointer(WM_LBUTTONUP,b); };
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(theme); HWND button=::GetDlgItem(editor.hwnd(),2003); Pump(180);
        const auto before=PresentedControl(button);
        ::SendMessageW(button,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(16,16));
        // Read the already presented pixels, WITHOUT servicing WM_PAINT. Native
        // BUTTON must not draw a foreign rectangle during state dispatch.
        CHECK(PresentedControl(button).pixels==before.pixels);
        Pump(180); Shot(button,theme==app::AppearanceTheme::Light ? L"held-button-light.png":L"held-button-dark.png");
        const auto held=PresentedControl(button);
        ::SendMessageW(button,WM_LBUTTONUP,0,MAKELPARAM(16,16));
        CHECK(PresentedControl(button).pixels==held.pixels);
        Pump(180);
        const auto enabled=PresentedControl(button); ::EnableWindow(button,FALSE);
        CHECK(PresentedControl(button).pixels==enabled.pixels); ::EnableWindow(button,TRUE);
    }
    for(auto tool:{ToolId::Rectangle,ToolId::Ellipse,ToolId::Line,ToolId::Arrow,ToolId::Pen,ToolId::Mosaic,ToolId::Spotlight,ToolId::Text,ToolId::Tag}) {
        Click(editor.hwnd(),2403); Click(editor.hwnd(),2000+static_cast<int>(tool));
        if(tool==ToolId::Mosaic) { Click(editor.context_hwnd(),2521); }
        if(tool==ToolId::Text || tool==ToolId::Tag) {
            click({160,180}); HWND host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"); HWND edit=::GetDlgItem(host,2700); CHECK(edit);
            ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Ghost-free text"));
            for(wchar_t character:std::wstring(L" typed")) {
                ::SendMessageW(edit,WM_CHAR,character,1); Pump(10);
                ::HideCaret(edit); const auto shown=PresentedControl(edit),expected=RenderControl(edit); ::ShowCaret(edit);
                CHECK(shown.pixels==expected.pixels);
            }
            click({800,500});
            click({170,190});
            HWND font=::GetDlgItem(editor.context_hwnd(),2600); ::SetWindowTextW(font,L"288 pt");
            ::SendMessageW(editor.context_hwnd(),WM_COMMAND,MAKEWPARAM(2600,CBN_KILLFOCUS),reinterpret_cast<LPARAM>(font));
            CHECK(editor.document().annotations()[0].style.font_size==288);
        } else { draw({160,180},{320,260}); }
        CHECK(editor.document().annotations().size()==1);
        const auto original=editor.document().annotations()[0];
        const POINT hit=tool==ToolId::Text || tool==ToolId::Tag ? POINT{170,190}:
            tool==ToolId::Rectangle || tool==ToolId::Ellipse || tool==ToolId::Spotlight || tool==ToolId::Mosaic ? POINT{240,180}:POINT{240,220};
        const auto committed=editor.Composite();
        for(POINT delta:{POINT{-10000,0},POINT{10000,0},POINT{0,-10000},POINT{0,10000}}) {
            draw(hit,{hit.x+delta.x,hit.y+delta.y});
            CHECK(editor.document().annotations()[0].points[0].x==original.points[0].x+delta.x);
            CHECK(editor.document().annotations()[0].points[0].y==original.points[0].y+delta.y);
            CHECK(RenderEditor(editor).pixels==source.pixels); // Selection frame is clipped too.
            CHECK(editor.Composite().pixels==source.pixels);
            Click(editor.hwnd(),2401); CHECK(editor.document().annotations()[0].Same(original));
            CHECK(editor.Composite().pixels==committed.pixels);
        }
    }
    editor.Destroy();
    ui::RecordingControls recording({}); CHECK(recording.ShowConfiguring(monitor.bounds,{}));
    RECT panel=Bounds(recording.hwnd());
    CHECK(std::abs((panel.top+panel.bottom)-(monitor.work_area.top+monitor.work_area.bottom))<=2);
    recording.SetElapsed(10'000'000);
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(theme);
        for(int i=0;i<3;++i) {
            recording.SetSessionState(app::SessionState::RecordingActive); Pump(180);
            CHECK(::SendMessageW(::GetDlgItem(recording.hwnd(),120),BM_GETCHECK,0,0)==BST_UNCHECKED);
            const auto pause=RenderControl(::GetDlgItem(recording.hwnd(),120));
            const auto normal=ui::theme::Current().text;
            const uint32_t normal_ink=0xff000000u|(GetRValue(normal)<<16)|(GetGValue(normal)<<8)|GetBValue(normal);
            CHECK(std::find(pause.pixels.begin(),pause.pixels.end(),normal_ink)!=pause.pixels.end());
            recording.SetSessionState(app::SessionState::RecordingPaused); Pump(180);
            CHECK(::SendMessageW(::GetDlgItem(recording.hwnd(),120),BM_GETCHECK,0,0)==BST_CHECKED);
            const auto resume=RenderControl(::GetDlgItem(recording.hwnd(),120));
            const auto selected=ui::theme::Current().focus;
            const uint32_t selected_ink=0xff000000u|(GetRValue(selected)<<16)|(GetGValue(selected)<<8)|GetBValue(selected);
            CHECK(std::find(resume.pixels.begin(),resume.pixels.end(),selected_ink)!=resume.pixels.end());
            const auto elapsed=RenderControl(::GetDlgItem(recording.hwnd(),101)); const auto color=ui::theme::Current().paused;
            const uint32_t ink=0xff000000u|(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
            CHECK(std::find(elapsed.pixels.begin(),elapsed.pixels.end(),ink)!=elapsed.pixels.end());
        }
    }
    recording.Destroy(); std::puts("Surface stability: held input, native typing, unrestricted/clipped movement, undo, centered placement and repeat pause states checked");
}
void InteractionCorrections() {
    using namespace editor;
    ScreenshotEditor editor({}); const RECT region=test::OnTestMonitor({40,40,940,600}); CHECK(editor.Open(region,nullptr,nullptr));
    platform::BgraImage source; source.Reset(900,560);
    for(int y=0;y<source.height;++y) for(int x=0;x<source.width;++x) { source.row(y)[x]=0xff000000|((x%128+48)<<16)|((y%128+48)<<8)|70; }
    CHECK(editor.SetImage(source)); Appearance(app::AppearanceTheme::Light);
    const auto tool=[&](ToolId value){ Click(editor.hwnd(),2000+static_cast<int>(value)); };
    const auto click=[&](POINT point){ point=test::OnTestPoint(point);editor.ImagePointer(WM_LBUTTONDOWN,point);editor.ImagePointer(WM_LBUTTONUP,point);};
    const auto draw=[&](POINT a,POINT b){ a=test::OnTestPoint(a);b=test::OnTestPoint(b);editor.ImagePointer(WM_LBUTTONDOWN,a);editor.ImagePointer(WM_MOUSEMOVE,b);editor.ImagePointer(WM_LBUTTONUP,b);};
    const auto canvas=[&](const wchar_t* name){if(!evidence.empty()){CHECK(platform::SaveImage(RenderEditor(editor),(evidence/name).wstring()));}};
    RECT bar{}; ::GetClientRect(editor.hwnd(),&bar); CHECK(bar.bottom<=static_cast<LONG>(50*::GetDpiForWindow(editor.hwnd())/96.0f));
    for(auto value:{ToolId::Rectangle,ToolId::Ellipse,ToolId::Line,ToolId::Arrow,ToolId::Pen,ToolId::Spotlight,ToolId::Mosaic}) {
        Click(editor.hwnd(),2403); tool(value);
        if(value==ToolId::Mosaic) { Click(editor.context_hwnd(),2521); }
        draw({140,160},{320,280}); CHECK(editor.document().annotations().size()==1);
        const auto unselected=RenderEditor(editor); tool(ToolId::Pointer); CHECK(RenderEditor(editor).pixels==unselected.pixels);
        const POINT hit=value==ToolId::Rectangle || value==ToolId::Ellipse || value==ToolId::Spotlight || value==ToolId::Mosaic ? POINT{230,160}:POINT{230,220};
        tool(value); CHECK(editor.ImageCursor(test::OnTestPoint(hit))==::LoadCursorW(nullptr,IDC_SIZEALL));
        click(hit); CHECK(RenderEditor(editor).pixels!=unselected.pixels);
        if(!evidence.empty()) {const auto name=L"selected-tool-"+std::to_wstring(static_cast<int>(value))+L".png";canvas(name.c_str());}
        const auto before=editor.document().annotations()[0]; draw(hit,{hit.x+30,hit.y+20});
        CHECK(editor.document().annotations().size()==1 && editor.document().annotations()[0].points[0].x==before.points[0].x+30);
        Click(editor.hwnd(),2401); CHECK(editor.document().annotations()[0].Same(before));
    }
    Click(editor.hwnd(),2403);
    for(auto value:{ToolId::Text,ToolId::Tag}) {
        tool(value); const auto count=editor.document().annotations().size(); click({150,160});
        HWND host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"),edit=::GetDlgItem(host,2700); CHECK(edit);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Hello")); Pump(20);
        GUITHREADINFO info{sizeof(info)}; CHECK(::GetGUIThreadInfo(::GetCurrentThreadId(),&info) && info.hwndCaret==edit);
        const auto edit_image=RenderControl(edit); RECT edit_bounds=Bounds(edit);
        const int source_x=edit_bounds.left-region.left,source_y=edit_bounds.top-region.top;
        CHECK(edit_image.row(0)[0]==source.row(source_y)[source_x]);
        Shot(edit,value==ToolId::Text ? L"text-native-edit.png":L"tag-native-edit.png");
        click({700,400}); CHECK(!::IsWindow(host) && editor.document().annotations().size()==count+1);
        const auto committed=RenderEditor(editor); tool(ToolId::Pointer); CHECK(RenderEditor(editor).pixels==committed.pixels);
        tool(value); click({700,400}); CHECK(editor.document().annotations().size()==count+2);
        editor.Keyboard(WM_KEYDOWN,VK_ESCAPE,0); CHECK(editor.document().annotations().size()==count+1);
        click({160,175}); CHECK(!::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"));
        const auto selected=editor.document().annotations().back();
        HWND font=::GetDlgItem(editor.context_hwnd(),2600); ::SetWindowTextW(font,L"48 pt");
        ::SendMessageW(editor.context_hwnd(),WM_COMMAND,MAKEWPARAM(2600,CBN_KILLFOCUS),reinterpret_cast<LPARAM>(font));
        CHECK(editor.document().annotations().back().style.font_size==48);
        ::SetWindowTextW(font,L"0 pt"); ::SendMessageW(editor.context_hwnd(),WM_COMMAND,MAKEWPARAM(2600,CBN_KILLFOCUS),reinterpret_cast<LPARAM>(font));
        CHECK(editor.document().annotations().back().style.font_size==48);
        HWND notice=::FindWindowW(L"EasnapCaptureCopiedNotice",L"Font size must be 6–288 pt"); CHECK(notice); if(notice) { ::SendMessageW(notice,WM_CLOSE,0,0); }
        editor.ImagePointer(WM_LBUTTONDBLCLK,test::OnTestPoint({165,180})); host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text");edit=::GetDlgItem(host,2700);CHECK(edit);
        const LONG old_width=Bounds(host).right-Bounds(host).left;
        ::SendMessageW(edit,EM_SETSEL,static_cast<WPARAM>(-1),-1); ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L" again"));
        CHECK(editor.document().annotations().back().text==L"Hello again" && Bounds(host).right-Bounds(host).left>old_width);
        // Drag the hollow frame while the native caret is active.
        POINT start{Bounds(edit).left+1,Bounds(edit).top+1};
        const auto before=editor.document().annotations().back();
        ::SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(1,1)); CHECK(!::IsWindow(host) && ::GetCapture()==editor.hwnd());
        POINT target{start.x+25,start.y+20}; ::ScreenToClient(editor.hwnd(),&target);
        ::SendMessageW(editor.hwnd(),WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(target.x,target.y));
        ::SendMessageW(editor.hwnd(),WM_LBUTTONUP,0,MAKELPARAM(target.x,target.y));
        CHECK(editor.document().annotations().back().points[0].x==before.points[0].x+25);
        canvas(value==ToolId::Text ? L"text-selected.png":L"tag-selected.png");
        Click(editor.hwnd(),2403);
    }
    tool(ToolId::Mosaic);
    HWND hidden_opacity=::GetDlgItem(editor.context_hwnd(),2602);
    CHECK((::GetWindowLongPtrW(hidden_opacity,GWL_STYLE)&WS_VISIBLE)==0);
    ::SendMessageW(hidden_opacity,TBM_SETPOS,TRUE,37);
    CHECK((::GetWindowLongPtrW(hidden_opacity,GWL_STYLE)&WS_VISIBLE)==0);
    Click(editor.context_hwnd(),2520);
    HWND popup=::FindWindowW(L"EasnapScreenshotEditor",L"Brush sizes"); CHECK(popup && ::IsWindowVisible(popup));
    POINT saved_cursor{}; ::GetCursorPos(&saved_cursor);
    Click(editor.context_hwnd(),2520); CHECK(!::IsWindowVisible(popup));
    HWND hand=::GetDlgItem(editor.context_hwnd(),2520); const auto hand_bounds=Bounds(hand);
    ::SetCursorPos((hand_bounds.left+hand_bounds.right)/2,(hand_bounds.top+hand_bounds.bottom)/2);
    ::SendMessageW(hand,WM_MOUSEMOVE,0,MAKELPARAM(10,10)); CHECK(WaitFor([&] { return ::IsWindowVisible(popup)!=FALSE; }));
    const auto work=platform::MonitorForRect(hand_bounds).work_area; ::SetCursorPos(work.right-2,work.bottom-2); Pump(320); CHECK(!::IsWindowVisible(popup));
    ::SetCursorPos(saved_cursor.x,saved_cursor.y); Click(editor.context_hwnd(),2520);
    Shot(popup,L"brush-mini-island.png"); Click(popup,2302); CHECK(!::IsWindowVisible(popup));
    Click(editor.context_hwnd(),2520); draw({140,160},{320,280});
    CHECK(!::IsWindowVisible(popup) && editor.document().annotations().back().style.brush_size==64);
    const auto unselected_brush=RenderEditor(editor); CHECK(editor.ImageCursor(test::OnTestPoint({212,247}))==::LoadCursorW(nullptr,IDC_SIZEALL));
    click({212,247}); CHECK(RenderEditor(editor).pixels!=unselected_brush.pixels);
    canvas(L"brush-selected.png");
    tool(ToolId::Spotlight); Shot(editor.context_hwnd(),L"context-spotlight-light.png");
    tool(ToolId::Watermark); HWND text=::GetDlgItem(editor.context_hwnd(),2604);
    CHECK(::SendMessageW(text,EM_GETLIMITTEXT,0,0)==16); ::SetWindowTextW(text,L"12345678901234567890");
    CHECK(editor.document().annotations().back().text==L"1234567890123456" && ::GetWindowTextLengthW(text)==16);
    ::SetWindowTextW(text,L"selam"); canvas(L"watermark-centered.png");
    for(auto preference:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(preference); tool(ToolId::Text); Pump(180);
        HWND font=::GetDlgItem(editor.context_hwnd(),2600); ::SendMessageW(font,CB_SHOWDROPDOWN,TRUE,0); Pump(20);
        COMBOBOXINFO combo{sizeof(combo)}; ::GetComboBoxInfo(font,&combo);
        CHECK(::GetWindowLongPtrW(font,GWL_STYLE)&CBS_OWNERDRAWFIXED);
        Shot(combo.hwndList,preference==app::AppearanceTheme::Light ? L"font-presets-light.png":L"font-presets-dark.png");
        ::SendMessageW(font,CB_SHOWDROPDOWN,FALSE,0);
        HWND button=::GetDlgItem(editor.hwnd(),2005); ::SendMessageW(button,BM_SETSTATE,TRUE,0); Pump(180);
        const auto pressed=RenderControl(button); ::SendMessageW(button,BM_SETSTATE,FALSE,0); Pump(180);
        CHECK(RenderControl(button).pixels==pressed.pixels);
    }
    MSG escape{}; escape.hwnd=::GetDlgItem(editor.hwnd(),2005);escape.message=WM_KEYDOWN;escape.wParam=VK_ESCAPE;
    CHECK(editor.ProcessDialogMessage(escape) && !editor.visible()); editor.Destroy();
    std::puts("Interaction corrections: direct selection, source-backed native text, frame dragging, compact popups, watermark limits and Escape checked");
}

void EditorFollowupInteractions() {
    using namespace editor;
    for(auto tool:{ToolId::Rectangle,ToolId::Ellipse,ToolId::Mosaic,ToolId::Spotlight}) {
        for(auto shape:{SpotlightShape::Rectangle,SpotlightShape::Ellipse,SpotlightShape::Rounded}) {
            Annotation a; a.tool=tool; a.points={{100,100},{300,300}}; a.style.method=EffectMethod::Region; a.style.shape=shape;
            CHECK(!a.HitTest({200,200})); CHECK(a.HitTest({200,100})); CHECK(!a.HitTest({200,70}));
        }
    }
    Annotation brush; brush.tool=ToolId::Mosaic; brush.points={{100,100},{300,100}}; brush.style.brush_size=64;
    CHECK(!brush.HitTest({200,100}) && brush.HitTest({200,132}) && brush.HitTest({68,100}));

    ScreenshotEditor editor({}); const RECT region=test::OnTestMonitor({40,40,940,600}); CHECK(editor.Open(region,nullptr,nullptr));
    platform::BgraImage source; source.Reset(900,560); std::fill(source.pixels.begin(),source.pixels.end(),0xff53718b);
    CHECK(editor.SetImage(source));
    const auto tool=[&](ToolId value){ Click(editor.hwnd(),2000+static_cast<int>(value)); };
    const auto click=[&](POINT p){ p=test::OnTestPoint(p); editor.ImagePointer(WM_LBUTTONDOWN,p); editor.ImagePointer(WM_LBUTTONUP,p); };
    const auto draw=[&](POINT a,POINT b){ a=test::OnTestPoint(a);b=test::OnTestPoint(b); editor.ImagePointer(WM_LBUTTONDOWN,a); editor.ImagePointer(WM_MOUSEMOVE,b); editor.ImagePointer(WM_LBUTTONUP,b); };
    for(auto value:{ToolId::Rectangle,ToolId::Ellipse,ToolId::Mosaic,ToolId::Spotlight}) {
        Click(editor.hwnd(),2403); tool(value);
        if(value==ToolId::Mosaic) { Click(editor.context_hwnd(),2521); }
        draw({140,140},{440,440}); const auto first=editor.document().annotations().front();
        draw({240,240},{340,340});
        CHECK(editor.document().annotations().size()==2 && editor.document().annotations().front().Same(first));
        draw({290,140},{320,140});
        CHECK(editor.document().annotations().front().points.front().x==first.points.front().x+30);
    }
    for(auto value:{ToolId::Text,ToolId::Tag}) {
        Click(editor.hwnd(),2403); tool(value); click({180,180});
        HWND host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"),edit=::GetDlgItem(host,2700); CHECK(edit);
        POINT caret_before{},caret_after{}; ::GetCaretPos(&caret_before);
        for(wchar_t ch:std::wstring(L"abc")) { ::SendMessageW(edit,WM_CHAR,ch,1); }
        ::GetCaretPos(&caret_after);
        CHECK(caret_after.x>caret_before.x);
        ::SendMessageW(edit,EM_SETSEL,0,-1);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Keep editing"));
        click({195,190}); CHECK(::IsWindow(host));
        const int end=::GetWindowTextLengthW(edit); ::SendMessageW(edit,EM_SETSEL,end,end);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L" here"));
        click({800,500});
        CHECK(!::IsWindow(host) && editor.document().annotations().size()==1 && editor.document().annotations()[0].text==L"Keep editing here");
    }
    Click(editor.hwnd(),2403); tool(ToolId::Rectangle);
    CHECK(::GetDlgItem(editor.context_hwnd(),2106) && ::GetDlgItem(editor.context_hwnd(),2107));
    Click(editor.context_hwnd(),2106); draw({140,140},{340,340});
    CHECK(editor.document().annotations().back().style.color.r==0 && editor.document().annotations().back().style.color.g==0);
    Click(editor.context_hwnd(),2107); draw({440,140},{640,340});
    CHECK(editor.document().annotations().back().style.color.r==.5f && editor.document().annotations().back().style.color.g==.5f);

    tool(ToolId::Mosaic); Click(editor.context_hwnd(),2520);
    HWND popup=::FindWindowW(L"EasnapScreenshotEditor",L"Brush sizes"); CHECK(popup && ::IsWindowVisible(popup));
    CHECK(!::GetDlgItem(editor.context_hwnd(),2522));
    ::SetWindowPos(editor.hwnd(),nullptr,100,100,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    RECT main=Bounds(editor.hwnd()),context=Bounds(editor.context_hwnd()),mini=Bounds(popup),hand=Bounds(::GetDlgItem(editor.context_hwnd(),2520));
    CHECK(context.top-main.bottom==mini.top-context.bottom);
    HRGN shape=::CreateRectRgn(0,0,0,0); CHECK(::GetWindowRgn(popup,shape)!=ERROR);
    CHECK(!::PtInRegion(shape,2,0)); CHECK(::PtInRegion(shape,(hand.left+hand.right)/2-mini.left,1)); ::DeleteObject(shape);
    Shot(popup,L"brush-callout-followup.png");
    const auto work=platform::MonitorForRect(main).work_area;
    Click(editor.context_hwnd(),2520); CHECK(!::IsWindowVisible(popup));
    ::SetWindowPos(editor.hwnd(),nullptr,main.left,work.bottom,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    const RECT closed_main=Bounds(editor.hwnd()),closed_context=Bounds(editor.context_hwnd());
    CHECK(closed_context.bottom==work.bottom);
    Click(editor.context_hwnd(),2520); CHECK(::IsWindowVisible(popup));
    main=Bounds(editor.hwnd()); context=Bounds(editor.context_hwnd()); mini=Bounds(popup); hand=Bounds(::GetDlgItem(editor.context_hwnd(),2520));
    const LONG gap=ui::theme::Scaled(8,::GetDpiForWindow(editor.hwnd())/96.0f);
    CHECK(context.top-main.bottom==gap && mini.top-context.bottom==gap);
    CHECK(main.top==closed_main.top-(mini.bottom-mini.top)-gap && mini.bottom==work.bottom);
    for(const RECT rect:{main,context,mini}) { CHECK(rect.left>=work.left && rect.top>=work.top && rect.right<=work.right && rect.bottom<=work.bottom); }
    CHECK((hand.left+hand.right)/2==(mini.left+mini.right)/2);
    shape=::CreateRectRgn(0,0,0,0); CHECK(::GetWindowRgn(popup,shape)!=ERROR);
    CHECK(::PtInRegion(shape,(hand.left+hand.right)/2-mini.left,1)); ::DeleteObject(shape);
    Shot(popup,L"brush-callout-bottom-edge.png");

    tool(ToolId::Watermark); HWND watermark=::GetDlgItem(editor.context_hwnd(),2604),clear=::GetDlgItem(watermark,2605);
    CHECK(watermark && clear && ::IsWindowVisible(watermark));
    CHECK(::SetWindowTextW(watermark,L"Clear me")); POINT saved{}; CHECK(::GetCursorPos(&saved));
    RECT field=Bounds(watermark);
    // Hover visibility uses the real screen cursor. Establish an actual entry
    // and dispatch its queued native input instead of injecting a different
    // WM_MOUSEMOVE coordinate immediately after an unchecked cursor move.
    CHECK(::SetCursorPos(field.left-20,(field.top+field.bottom)/2));
    Pump(10); // Keep the outside and inside moves from coalescing into one.
    CHECK(WaitFor([&] { return !::IsWindowVisible(clear); }));
    field=Bounds(watermark); const POINT entry{(field.left+field.right)/2,(field.top+field.bottom)/2};
    CHECK(::SetCursorPos(entry.x,entry.y));
    POINT actual{}; CHECK(::GetCursorPos(&actual) && actual.x==entry.x && actual.y==entry.y);
    const HWND hit=::WindowFromPoint(entry); CHECK(hit==watermark || ::IsChild(watermark,hit));
    const bool revealed=WaitFor([&] { return ::IsWindowVisible(clear)!=FALSE; });
    if(!revealed) {
        const RECT current_field=Bounds(watermark); ::GetCursorPos(&actual);
        std::printf("Watermark hover cursor=%ld,%ld field=%ld,%ld,%ld,%ld context_visible=%d input_visible=%d text_length=%d hit=%p\n",
            actual.x,actual.y,current_field.left,current_field.top,current_field.right,current_field.bottom,
            ::IsWindowVisible(editor.context_hwnd()),::IsWindowVisible(watermark),::GetWindowTextLengthW(watermark),
            static_cast<void*>(::WindowFromPoint(actual)));
    }
    CHECK(revealed);
    Shot(clear,L"watermark-clear-button-followup.png");
    Shot(editor.context_hwnd(),L"watermark-clear-followup.png");
    for(auto preference:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(preference); Pump(180);
        Shot(editor.context_hwnd(),preference==app::AppearanceTheme::Light ? L"watermark-clear-light.png":L"watermark-clear-dark.png");
    }
    Click(watermark,2605); CHECK(::GetWindowTextLengthW(watermark)==0 && editor.document().annotations().back().text.empty());
    Click(editor.hwnd(),2401); CHECK(editor.document().annotations().back().text==L"Clear me");
    ::SetFocus(clear);
    CHECK(::SetCursorPos(field.right+30,field.top));
    ::SendMessageW(clear,WM_MOUSELEAVE,0,0); Pump(100);
    CHECK(!::IsWindowVisible(clear));
    field=Bounds(watermark);
    CHECK(::SetCursorPos(field.left+20,field.top+10));
    CHECK(::GetCursorPos(&actual) && ::PtInRect(&field,actual));
    CHECK(WaitFor([&] { return ::IsWindowVisible(clear)!=FALSE; }));
    CHECK(::GetWindowTextLengthW(watermark)==8);
    CHECK(::SetCursorPos(field.right+30,field.top)); Pump(100);
    CHECK(WaitFor([&] { return !::IsWindowVisible(clear); })); // Timer also handles leaving the app entirely.
    CHECK(::SetCursorPos(saved.x,saved.y));

    struct SaveProbe { HWND context{},popup{}; bool seen=false,hidden=false; } probe{editor.context_hwnd(),popup};
    static SaveProbe* active_probe=nullptr; active_probe=&probe;
    tool(ToolId::Mosaic); Click(editor.context_hwnd(),2520); CHECK(::IsWindowVisible(popup));
    ::SetTimer(editor.hwnd(),0xEA90,25,[](HWND owner,UINT,UINT_PTR timer,DWORD) {
        HWND dialog=::GetLastActivePopup(owner); wchar_t cls[32]{}; ::GetClassNameW(dialog,cls,32);
        if(dialog!=owner && ::lstrcmpW(cls,L"#32770")==0 && ::IsWindowVisible(dialog)) {
            active_probe->seen=true;
            active_probe->hidden=!::IsWindowVisible(active_probe->context) && !::IsWindowVisible(active_probe->popup);
            ::KillTimer(owner,timer); ::PostMessageW(dialog,WM_CLOSE,0,0);
        }
    });
    Click(editor.hwnd(),2405); ::KillTimer(editor.hwnd(),0xEA90); active_probe=nullptr;
    CHECK(probe.seen && probe.hidden && editor.visible() && ::IsWindowVisible(editor.context_hwnd()) && !::IsWindowVisible(popup));
    editor.Destroy();
    std::puts("Editor follow-up: outline-only hit regions, nested shapes, repeated text clicks, black/gray, joined brush popup, watermark clear/Undo and native Save priority checked");
}

void TextDispatchPainting() {
    using namespace editor;
    ScreenshotEditor editor({}); CHECK(editor.Open(test::OnTestMonitor({40,40,940,700}),nullptr,nullptr));
    platform::BgraImage source; source.Reset(900,660); std::fill(source.pixels.begin(),source.pixels.end(),0xff53718b);
    CHECK(editor.SetImage(source));
    const bool clipboard_open=::OpenClipboard(editor.hwnd())!=FALSE;
    CHECK(clipboard_open);
    if(clipboard_open) {
        constexpr wchar_t paste[]=L"Paste";
        HGLOBAL data=::GlobalAlloc(GMEM_MOVEABLE,sizeof(paste)); CHECK(data);
        auto* text=data ? static_cast<wchar_t*>(::GlobalLock(data)):nullptr; CHECK(text);
        if(text) {
            std::copy(std::begin(paste),std::end(paste),text); ::GlobalUnlock(data);
            if(::EmptyClipboard() && ::SetClipboardData(CF_UNICODETEXT,data)) { data=nullptr; }
            else { CHECK(false); }
        }
        if(data) { ::GlobalFree(data); }
        ::CloseClipboard();
    }
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(theme);
        for(int mode:{2500,2501,2502}) {
            Click(editor.hwnd(),2403); Click(editor.hwnd(),2000+static_cast<int>(ToolId::Text));
            Click(editor.context_hwnd(),mode);
            editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({160,180})); editor.ImagePointer(WM_LBUTTONUP,test::OnTestPoint({160,180}));
            HWND host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"),edit=::GetDlgItem(host,2700); CHECK(edit);
            ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Color\r\nMMMMMMMMMMMMMMMMMMMM"));
            for(bool reopened:{false,true}) {
                if(reopened) {
                    editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({780,570})); editor.ImagePointer(WM_LBUTTONUP,test::OnTestPoint({780,570}));
                    editor.ImagePointer(WM_LBUTTONDBLCLK,test::OnTestPoint({180,195}));
                    host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"); edit=::GetDlgItem(host,2700); CHECK(edit);
                }
                bool recorded=false;
                const auto caret_bounds=[&] {
                    GUITHREADINFO info{sizeof(info)};
                    CHECK(::GetGUIThreadInfo(::GetCurrentThreadId(),&info));
                    CHECK(info.hwndCaret==edit);
                    return info.rcCaret;
                };
                const auto dispatch=[&](UINT message,WPARAM wparam,LPARAM lparam) {
                    Pump(10); const RECT old_caret=caret_bounds();
                    ::HideCaret(edit); const auto before=PresentedControl(edit); ::ShowCaret(edit);
                    const RECT bounds=Bounds(edit);
                    ::SendMessageW(edit,message,wparam,lparam);
                    // Sample before servicing WM_PAINT: either the previous custom
                    // frame or the new one is coherent, native glyphs/backing are not.
                    ::HideCaret(edit); const auto shown=PresentedControl(edit),expected=RenderControl(edit); ::ShowCaret(edit);
                    const RECT new_caret=caret_bounds();
                    if(!recorded && !evidence.empty()) {
                        const auto prefix=std::wstring(theme==app::AppearanceTheme::Light ? L"text-light-":L"text-dark-")+
                            std::to_wstring(mode)+(reopened ? L"-reopened-":L"-new-");
                        CHECK(platform::SaveImage(before,(evidence/(prefix+L"before.png")).wstring()));
                        CHECK(platform::SaveImage(shown,(evidence/(prefix+L"dispatch.png")).wstring()));
                        CHECK(platform::SaveImage(expected,(evidence/(prefix+L"expected.png")).wstring()));
                        recorded=true;
                    }
                    CHECK(Same(bounds,Bounds(edit)));
                    const auto same_content=[&](const platform::BgraImage& frame) {
                        if(shown.width!=frame.width || shown.height!=frame.height) { return false; }
                        for(int y=0;y<shown.height;++y) for(int x=0;x<shown.width;++x) {
                            const POINT point{x,y};
                            // The native caret may move/blink synchronously. Only
                            // its two OS-reported rectangles are excluded.
                            if(::PtInRect(&old_caret,point) || ::PtInRect(&new_caret,point)) { continue; }
                            if(shown.row(y)[x]!=frame.row(y)[x]) { return false; }
                        }
                        return true;
                    };
                    CHECK(same_content(before) || same_content(expected));
                    // WM_PAINT is asynchronous. Wait for the settled frame with a
                    // deadline; one fixed pump is a timing assumption that fails
                    // when the machine is busy. The settled frame itself is exact.
                    RECT settled_caret{},difference_bounds{};
                    platform::BgraImage settled,settled_expected;
                    bool same_size=false;
                    size_t differences=0,content_differences=0;
                    const auto settle_deadline=::GetTickCount64()+1500;
                    for(;;) {
                        Pump(10); settled_caret=caret_bounds();
                        ::HideCaret(edit); settled=PresentedControl(edit); settled_expected=RenderControl(edit); ::ShowCaret(edit);
                        same_size=settled.width==settled_expected.width && settled.height==settled_expected.height;
                        differences=content_differences=0;
                        difference_bounds={settled.width,settled.height,0,0};
                        if(same_size) {
                            for(int y=0;y<settled.height;++y) for(int x=0;x<settled.width;++x) {
                                if(settled.row(y)[x]==settled_expected.row(y)[x]) { continue; }
                                ++differences;
                                difference_bounds.left=std::min(difference_bounds.left,static_cast<LONG>(x));
                                difference_bounds.top=std::min(difference_bounds.top,static_cast<LONG>(y));
                                difference_bounds.right=std::max(difference_bounds.right,static_cast<LONG>(x+1));
                                difference_bounds.bottom=std::max(difference_bounds.bottom,static_cast<LONG>(y+1));
                                // After WM_PAINT only the current OS caret can vary;
                                // old caret pixels and all glyph/backing pixels must
                                // exactly match the source-backed custom renderer.
                                if(!::PtInRect(&settled_caret,POINT{x,y})) { ++content_differences; }
                            }
                        }
                        if((same_size && content_differences==0) || ::GetTickCount64()>=settle_deadline) { break; }
                    }
                    CHECK(Same(settled_caret,caret_bounds()));
                    if(differences || !same_size) {
                        std::printf("Text settled mode=%d theme=%d reopened=%d message=%u size_match=%d differences=%zu outside_caret=%zu bounds=%ld,%ld,%ld,%ld caret=%ld,%ld,%ld,%ld\n",
                            mode,static_cast<int>(theme),reopened,message,same_size,differences,content_differences,
                            difference_bounds.left,difference_bounds.top,difference_bounds.right,difference_bounds.bottom,
                            settled_caret.left,settled_caret.top,settled_caret.right,settled_caret.bottom);
                        if(!evidence.empty()) {
                            const auto prefix=std::wstring(theme==app::AppearanceTheme::Light ? L"text-light-":L"text-dark-")+
                                std::to_wstring(mode)+(reopened ? L"-reopened-":L"-new-")+std::to_wstring(message);
                            CHECK(platform::SaveImage(settled,(evidence/(prefix+L"-settled.png")).wstring()));
                            CHECK(platform::SaveImage(settled_expected,(evidence/(prefix+L"-settled-expected.png")).wstring()));
                        }
                    }
                    CHECK(same_size && content_differences==0);
                };
                ::SendMessageW(edit,EM_SETSEL,0,1); dispatch(WM_CHAR,L'R',1);
                ::SendMessageW(edit,EM_SETSEL,1,1); dispatch(WM_CHAR,L'X',1);
                dispatch(WM_CHAR,VK_BACK,1);
                ::SendMessageW(edit,EM_SETSEL,0,1); dispatch(EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Blue"));
                ::SendMessageW(edit,EM_SETSEL,0,0); dispatch(WM_KEYDOWN,VK_DELETE,1);
                ::SendMessageW(edit,EM_SETSEL,0,1); dispatch(WM_PASTE,0,0);
                CHECK(editor.document().annotations().back().text.rfind(L"Paste",0)==0);
            }
            editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({780,570})); editor.ImagePointer(WM_LBUTTONUP,test::OnTestPoint({780,570}));
        }
    }
    editor.Destroy();
}

void StandardTextFlows() {
    using namespace editor;
    TextDispatchPainting();
    ScreenshotEditor* active{};
    ui::OverlayController::Callbacks overlay_callbacks;
    overlay_callbacks.uses_annotation_tool=[&] { return active && active->uses_annotation_tool(); };
    overlay_callbacks.wants_annotation_pointer=[&](POINT point) { return active && active->WantsPointer(point); };
    overlay_callbacks.on_image_pointer=[&](UINT message,POINT point) { active->ImagePointer(message,point); };
    overlay_callbacks.on_editor_key=[&](UINT message,WPARAM key,LPARAM flags) { active->Keyboard(message,key,flags); };
    overlay_callbacks.on_paint_image=[&](HDC dc,const RECT& bounds,const RECT& repaint) {
        if(active) { active->RenderImage(dc,bounds,&repaint); }
    };
    ui::OverlayController overlay(overlay_callbacks);
    capture::DesktopSnapshot snapshot; snapshot.virtual_bounds=platform::VirtualScreenBounds();
    CHECK(overlay.Show(snapshot,test::OnTestPoint({200,200})));
    const RECT region=test::OnTestMonitor({40,40,940,700}); overlay.selector().SetSelection(region); overlay.EnterEditing();
    ScreenshotEditor::Callbacks callbacks;
    callbacks.on_feature_used=[&] { overlay.workspace().EditorFeatureUsed(); };
    callbacks.on_document_committed=[&] { static_cast<void>(overlay.workspace().DocumentCommitted()); };
    callbacks.on_image_invalidated=[&](const RECT& damage) { overlay.InvalidateImage(damage); };
    ScreenshotEditor editor(callbacks); active=&editor; CHECK(editor.Open(region,nullptr,overlay.owner_window()));
    platform::BgraImage source; source.Reset(900,660); std::fill(source.pixels.begin(),source.pixels.end(),0xff53718b);
    CHECK(editor.SetImage(source)); overlay.FixScreenshot();
    const auto pointer=[&](UINT message,POINT p) {
        p=test::OnTestPoint(p);
        ::ScreenToClient(overlay.owner_window(),&p);
        ::SendMessageW(overlay.owner_window(),message,message==WM_LBUTTONUP ? 0:MK_LBUTTON,MAKELPARAM(p.x,p.y));
    };
    const auto click=[&](POINT p) { pointer(WM_LBUTTONDOWN,p); pointer(WM_LBUTTONUP,p); };
    const auto edit_window=[] { return ::GetDlgItem(::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"),2700); };
    const auto selection=[](HWND edit) { DWORD a{},b{}; ::SendMessageW(edit,EM_GETSEL,reinterpret_cast<WPARAM>(&a),reinterpret_cast<LPARAM>(&b)); return std::pair{a,b}; };
    const auto key=[&](HWND edit,WPARAM value,bool control=false,bool shift=false) {
        std::array<BYTE,256> old{},keys{}; ::GetKeyboardState(old.data());
        if(control) { keys[VK_CONTROL]=0x80; } if(shift) { keys[VK_SHIFT]=0x80; }
        ::SetKeyboardState(keys.data()); ::SendMessageW(edit,WM_KEYDOWN,value,1); ::SetKeyboardState(old.data());
    };
    const auto point=[](HWND edit,int index) { const auto p=::SendMessageW(edit,EM_POSFROMCHAR,index,0); return MAKELPARAM(GET_X_LPARAM(p)+2,GET_Y_LPARAM(p)+8); };
    for(auto tool:{ToolId::Text,ToolId::Tag}) {
        Click(editor.hwnd(),2403); Click(editor.hwnd(),2000+static_cast<int>(tool)); click({160,180}); HWND edit=edit_window(); CHECK(edit);
        pointer(WM_LBUTTONDBLCLK,{160,180});
        CHECK(edit_window()==edit && editor.document().annotations().size()==1);
        ::SendMessageW(edit,WM_LBUTTONUP,0,point(edit,0));
        for(wchar_t ch:std::wstring(L"Alpha beta")) { ::SendMessageW(edit,WM_CHAR,ch,1); }
        POINT first{}; CHECK(::GetCaretPos(&first)); CHECK(first.x>40);
        ::SendMessageW(edit,WM_CHAR,L'\r',1);
        for(wchar_t ch:std::wstring(L"Second line")) { ::SendMessageW(edit,WM_CHAR,ch,1); }
        CHECK(editor.document().annotations()[0].text==L"Alpha beta\r\nSecond line");
        POINT second{}; CHECK(::GetCaretPos(&second)); CHECK(second.y>first.y);
        key(edit,VK_HOME); CHECK(selection(edit).first==12);
        key(edit,VK_END); CHECK(selection(edit).first==23);
        key(edit,VK_LEFT,false,true); CHECK(selection(edit)==std::pair(DWORD{22},DWORD{23}));
        key(edit,VK_HOME,true); CHECK(selection(edit).first==0);
        key(edit,'A',true); CHECK(selection(edit)==std::pair(DWORD{0},DWORD{23}));
        // Single-click insertion and native drag selection, in both directions.
        auto a=point(edit,2),b=point(edit,8);
        ::SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,a); ::SendMessageW(edit,WM_LBUTTONUP,0,a);
        CHECK(selection(edit).first==selection(edit).second && selection(edit).first>=2 && selection(edit).first<=3);
        ::SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,a); ::SendMessageW(edit,WM_MOUSEMOVE,MK_LBUTTON,b); ::SendMessageW(edit,WM_LBUTTONUP,0,b);
        CHECK(selection(edit).second>selection(edit).first);
        ::SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,b); ::SendMessageW(edit,WM_MOUSEMOVE,MK_LBUTTON,a); ::SendMessageW(edit,WM_LBUTTONUP,0,a);
        CHECK(selection(edit).second>selection(edit).first);
        Pump(20);
        ::SendMessageW(edit,WM_LBUTTONDBLCLK,MK_LBUTTON,point(edit,1)); ::SendMessageW(edit,WM_LBUTTONUP,0,point(edit,1));
        CHECK(selection(edit).first==0 && selection(edit).second>=5 && selection(edit).second<=6);
        ::SendMessageW(edit,WM_LBUTTONDOWN,MK_LBUTTON,point(edit,1)); ::SendMessageW(edit,WM_LBUTTONUP,0,point(edit,1));
        CHECK(selection(edit)==std::pair(DWORD{0},DWORD{12}));
        // Replacement, native undo, and re-edit font reuse.
        ::SendMessageW(edit,EM_EMPTYUNDOBUFFER,0,0);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Replaced\r\n"));
        CHECK(editor.document().annotations()[0].text==L"Replaced\r\nSecond line");
        ::SendMessageW(edit,WM_UNDO,0,0); CHECK(editor.document().annotations()[0].text==L"Alpha beta\r\nSecond line");
        click({780,570}); CHECK(!edit_window() && editor.document().annotations().size()==1);
        click({500,360}); edit=edit_window(); CHECK(edit && editor.document().annotations().size()==2);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Other box"));
        click({180,195}); edit=edit_window(); CHECK(edit && editor.document().annotations().size()==2);
        wchar_t value[64]{}; ::GetWindowTextW(edit,value,64); CHECK(std::wstring(value)==L"Alpha beta\r\nSecond line");
        HFONT font=reinterpret_cast<HFONT>(::SendMessageW(edit,WM_GETFONT,0,0)); LOGFONTW details{};
        CHECK(font && ::GetObjectW(font,sizeof(details),&details) && std::abs(details.lfHeight)>16);
        click({530,375}); edit=edit_window(); ::GetWindowTextW(edit,value,64); CHECK(std::wstring(value)==L"Other box");
        click({780,570}); CHECK(!edit_window() && editor.document().annotations().size()==2);
        // Windows replaces the second DOWN with DBLCLK. The first blank press
        // ended editing; its double-click continuation must not create a box.
        pointer(WM_LBUTTONDBLCLK,{780,570});
        pointer(WM_LBUTTONUP,{780,570});
        CHECK(!edit_window() && editor.document().annotations().size()==2);
        // Empty active boxes may be removed before hit indices are recalculated.
        click({720,490}); CHECK(editor.document().annotations().size()==3);
        click({180,195}); CHECK(edit_window() && editor.document().annotations().size()==2);
        click({780,570});
        // Inactive single clicks select/drag. Double click reopens native editing.
        click({180,195}); CHECK(!edit_window());
        pointer(WM_LBUTTONDBLCLK,{180,195}); edit=edit_window(); CHECK(edit && ::GetFocus()==edit);
        key(edit,VK_ESCAPE); CHECK(!edit_window() && editor.document().annotations().size()==2);
        // A chain of existing targets must retain the edit session at each step.
        click({700,300}); edit=edit_window(); CHECK(edit);
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Third box"));
        for(POINT target:{POINT{180,195},POINT{530,375},POINT{720,310},POINT{180,195}}) {
            click(target); CHECK(edit_window() && ::GetFocus()==edit_window());
            CHECK(editor.document().annotations().size()==3);
        }
        click({780,570}); CHECK(!edit_window() && editor.document().annotations().size()==3);
        click({780,570}); CHECK(edit_window() && editor.document().annotations().size()==4);
        key(edit_window(),VK_ESCAPE); CHECK(!edit_window() && editor.document().annotations().size()==3);
    }
    // Deliver actual desktop input as well: SendMessage to the overlay bypasses
    // native child hit testing, activation and the OS double-click conversion.
    Click(editor.hwnd(),2403); Click(editor.hwnd(),2000+static_cast<int>(ToolId::Text));
    POINT saved_cursor{}; CHECK(::GetCursorPos(&saved_cursor));
    const auto physical_click=[&](POINT p) {
        p=test::OnTestPoint(p);
        CHECK(::SetCursorPos(p.x,p.y));
        Pump(10);
        const HWND target=::WindowFromPoint(p);
        const bool owned=target && ::GetWindowThreadProcessId(target,nullptr)==::GetCurrentThreadId();
        CHECK(owned);
        if(!owned) { return false; } // Never send input into another application.
        INPUT input[2]{}; input[0].type=input[1].type=INPUT_MOUSE;
        input[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; input[1].mi.dwFlags=MOUSEEVENTF_LEFTUP;
        CHECK(::SendInput(2,input,sizeof(INPUT))==2); Pump(30);
        return true;
    };
    const POINT blank{800,550};
    for(POINT p:{POINT{300,250},POINT{500,350},POINT{700,350},POINT{150,170}}) {
        if(!physical_click(p)) { break; }
        HWND edit=edit_window(); CHECK(edit && ::GetFocus()==edit);
        if(!edit) { break; }
        ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Existing text"));
        if(p.x!=150) { physical_click(blank); CHECK(!edit_window()); }
    }
    CHECK(editor.document().annotations().size()==4);
    for(POINT target:{POINT{320,260},POINT{520,360},POINT{720,360},POINT{170,180}}) {
        physical_click(target); CHECK(edit_window() && ::GetFocus()==edit_window());
        HWND edit=edit_window();
        if(edit && ::GetFocus()==edit && ::GetWindowThreadProcessId(edit,nullptr)==::GetCurrentThreadId()) {
            INPUT input[2]{}; input[0].type=input[1].type=INPUT_KEYBOARD;
            input[0].ki.wScan=input[1].ki.wScan=L'x';
            input[0].ki.dwFlags=KEYEVENTF_UNICODE; input[1].ki.dwFlags=KEYEVENTF_UNICODE|KEYEVENTF_KEYUP;
            CHECK(::SendInput(2,input,sizeof(INPUT))==2); Pump(30);
            CHECK(edit_window()==edit && ::GetFocus()==edit);
        }
        CHECK(editor.document().annotations().size()==4);
    }
    physical_click(blank); CHECK(!edit_window() && editor.document().annotations().size()==4);
    Pump(::GetDoubleClickTime()+10); // A later ordinary press, not DBLCLK continuation.
    physical_click(blank); CHECK(edit_window() && editor.document().annotations().size()==5);
    if(edit_window()) { key(edit_window(),VK_ESCAPE); }
    // Selected text, including after a drag, also owns the next blank press.
    // Repeated selection changes do not alternate a hidden creation flag.
    physical_click({320,260}); CHECK(!edit_window() && editor.document().annotations().size()==4);
    const auto original=editor.document().annotations()[0].points.front();
    Pump(::GetDoubleClickTime()+10);
    const POINT drag_start=test::OnTestPoint({320,260}),drag_end=test::OnTestPoint({350,280});
    CHECK(::SetCursorPos(drag_start.x,drag_start.y));
    const bool owns_drag=::GetWindowThreadProcessId(::WindowFromPoint(drag_start),nullptr)==::GetCurrentThreadId();
    CHECK(owns_drag);
    if(owns_drag) {
        INPUT press{}; press.type=INPUT_MOUSE; press.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
        CHECK(::SendInput(1,&press,sizeof(INPUT))==1); Pump(10);
        CHECK(::SetCursorPos(drag_end.x,drag_end.y)); Pump(10);
        press.mi.dwFlags=MOUSEEVENTF_LEFTUP; CHECK(::SendInput(1,&press,sizeof(INPUT))==1); Pump(30);
    }
    CHECK(!edit_window() && editor.document().annotations().size()==4);
    CHECK(editor.document().annotations()[0].points.front().x==original.x+30 &&
          editor.document().annotations()[0].points.front().y==original.y+20);
    for(POINT target:{POINT{520,360},POINT{720,360},POINT{170,180},POINT{350,280}}) {
        physical_click(target); CHECK(!edit_window() && editor.document().annotations().size()==4);
    }
    physical_click(blank); CHECK(!edit_window() && editor.document().annotations().size()==4);
    Pump(::GetDoubleClickTime()+10);
    physical_click(blank); CHECK(edit_window() && editor.document().annotations().size()==5);
    if(edit_window()) { key(edit_window(),VK_ESCAPE); }
    physical_click({350,280}); CHECK(!edit_window());
    physical_click({350,280}); CHECK(edit_window() && editor.document().annotations().size()==4);
    if(edit_window()) { key(edit_window(),VK_ESCAPE); }
    CHECK(::SetCursorPos(saved_cursor.x,saved_cursor.y));
    editor.Destroy(); overlay.Destroy();
    std::puts("Standard text: caret, newlines, navigation, single/double/triple click, drag, replacement/undo, box switching and Escape checked");
}

void AnnotationEditing() {
    using namespace editor;
    AnnotationDocument document;
    Annotation line; line.tool=ToolId::Line; line.points={{20,20},{100,80}};
    document.Apply(line); CHECK(document.HitTest({50,42}).has_value()); CHECK(!document.HitTest({90,25}));
    CHECK(line.Handles().size()==2);
    document.BeginTransaction();
    for(int i=0;i<20;++i) { auto moved=line; moved.points[0].x+=i; document.Replace(0,moved); }
    document.CancelTransaction(); CHECK(document.annotations()[0].Same(line));
    document.BeginTransaction(); auto moved=line; moved.points[0].x=40; document.Replace(0,moved); document.CommitTransaction();
    CHECK(document.Undo() && document.annotations()[0].Same(line)); CHECK(document.Redo() && document.annotations()[0].Same(moved));
    document.Clear(); CHECK(document.annotations().empty()); CHECK(document.Undo() && document.annotations().size()==1);
    for(auto tool:{ToolId::Rectangle,ToolId::Ellipse,ToolId::Spotlight,ToolId::Mosaic}) {
        Annotation a=line; a.tool=tool; a.style.method=EffectMethod::Region; CHECK(a.Handles().size()==4);
    }
    for(auto tool:{ToolId::Pen,ToolId::Text,ToolId::Tag,ToolId::Watermark}) { Annotation a=line; a.tool=tool; CHECK(a.Handles().empty()); }

    ScreenshotEditor editor({}); CHECK(editor.Open(test::OnTestMonitor({40,40,840,520}),nullptr,nullptr));
    platform::BgraImage source; source.Reset(800,480);
    for(int y=0;y<480;++y) for(int x=0;x<800;++x) { source.row(y)[x]=((x/10+y/10)%2) ? 0xffd0d8e0u:0xff506070u; }
    CHECK(editor.SetImage(source));
    const auto tool=[&](ToolId value) { Click(editor.hwnd(),2000+static_cast<int>(value)); };
    const auto option=[&](int id) { Click(editor.context_hwnd(),id); };
    const auto draw=[&](POINT a,POINT b) { a=test::OnTestPoint(a);b=test::OnTestPoint(b); editor.ImagePointer(WM_LBUTTONDOWN,a);editor.ImagePointer(WM_MOUSEMOVE,b);editor.ImagePointer(WM_LBUTTONUP,b); };
    const auto undo=[&] { Click(editor.hwnd(),2401); };
    const auto redo=[&] { Click(editor.hwnd(),2402); };
    const auto clear=[&] { Click(editor.hwnd(),2403); };
    const auto finish_text=[&] {
        std::array<BYTE,256> keys{};keys[VK_CONTROL]=0x80;::SetKeyboardState(keys.data());
        editor.Keyboard(WM_KEYDOWN,VK_RETURN,0); keys.fill(0);::SetKeyboardState(keys.data());
    };
    const auto capture=[&](const wchar_t* name) {
        if(evidence.empty()) { return; }
        auto image=editor.Composite(); CHECK(platform::SaveImage(image,(evidence/name).wstring()));
    };
    tool(ToolId::Text); editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({90,90}));
    HWND host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"); CHECK(host);
    HWND edit=::GetDlgItem(host,2700); CHECK(edit && ::GetFocus()==edit);
    ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Alpha beta\r\nSecond paragraph"));
    CHECK(editor.document().annotations().back().text==L"Alpha beta\r\nSecond paragraph");
    finish_text(); CHECK(!::IsWindow(host));
    const auto text=editor.document().annotations().back(); CHECK(text.bounds().right>text.points[0].x+50); CHECK(text.Handles().empty());
    tool(ToolId::Pointer); editor.ImagePointer(WM_LBUTTONDBLCLK,test::OnTestPoint({100,100}));
    host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text"); edit=::GetDlgItem(host,2700); CHECK(edit);
    ::SendMessageW(edit,EM_SETSEL,0,5);
    HWND font=::GetDlgItem(editor.context_hwnd(),2600); ::SendMessageW(font,CB_SETCURSEL,3,0);
    ::SendMessageW(editor.context_hwnd(),WM_COMMAND,MAKEWPARAM(2600,CBN_SELCHANGE),reinterpret_cast<LPARAM>(font));
    CHECK(editor.document().annotations().back().style.font_size==36);
    CHECK(editor.document().annotations().back().text==text.text);
    option(2502); CHECK(editor.document().annotations().back().style.text_mode==TextMode::Outline);
    option(2105); CHECK(editor.document().annotations().back().style.color.r==1);
    finish_text(); capture(L"text-outline-white-export.png");
    const auto before_move=editor.Composite(); draw({100,100},{130,120});
    CHECK(editor.document().annotations().back().points[0].x==text.points[0].x+30);
    undo(); CHECK(editor.Composite().pixels==before_move.pixels); redo();
    tool(ToolId::Tag); editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({450,100}));
    host=::FindWindowW(L"EasnapScreenshotEditor",L"Edit annotation text");edit=::GetDlgItem(host,2700); CHECK(edit);
    ::SendMessageW(edit,EM_REPLACESEL,TRUE,reinterpret_cast<LPARAM>(L"Review")); finish_text(); CHECK(editor.document().annotations().back().Handles().empty());
    const auto tag_before=editor.Composite(); Appearance(app::AppearanceTheme::Dark); CHECK(editor.Composite().pixels==tag_before.pixels);
    tool(ToolId::Line); draw({150,300},{380,380}); CHECK(editor.document().annotations().back().Handles().size()==2);
    const auto line_before=editor.Composite(); draw({380,380},{400,410}); CHECK(editor.Composite().pixels!=line_before.pixels);
    undo(); CHECK(editor.Composite().pixels==line_before.pixels);
    capture(L"annotations-export.png");
    clear(); tool(ToolId::Mosaic); option(2521); draw({120,140},{300,300});
    CHECK(editor.document().annotations().back().style.method==EffectMethod::Region);
    const auto blurred=editor.Composite(); CHECK(blurred.pixels!=source.pixels);
    tool(ToolId::Pointer); draw({120,200},{420,200});
    const auto relocated=editor.Composite(); CHECK(relocated.pixels!=blurred.pixels);
    CHECK(relocated.row(120)[100]==source.row(120)[100]); // No remnants in the original region.
    undo(); CHECK(editor.Composite().pixels==blurred.pixels); redo();
    editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({420,200})); editor.ImagePointer(WM_MOUSEMOVE,test::OnTestPoint({470,230})); editor.ImagePointer(WM_CAPTURECHANGED,test::OnTestPoint({470,230}));
    CHECK(editor.Composite().pixels==relocated.pixels);
    option(2511); CHECK(editor.document().annotations().back().style.effect==EffectMode::Pixelate);
    const auto pixelated=editor.Composite(); CHECK(pixelated.pixels!=relocated.pixels);
    option(2510); CHECK(editor.Composite().pixels==relocated.pixels);
    HWND strength=::GetDlgItem(editor.context_hwnd(),2601);
    ::SendMessageW(strength,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,16));
    for(int value:{60,70,90}) { ::SendMessageW(strength,TBM_SETPOS,TRUE,value); ::SendMessageW(editor.context_hwnd(),WM_HSCROLL,TB_THUMBTRACK,reinterpret_cast<LPARAM>(strength)); }
    ::SendMessageW(strength,WM_LBUTTONUP,0,MAKELPARAM(20,16));
    undo(); CHECK(editor.Composite().pixels==relocated.pixels);
    clear(); tool(ToolId::Spotlight); option(2531); draw({100,100},{300,300});
    tool(ToolId::Spotlight); draw({400,100},{600,300});
    auto spots=editor.Composite(); CHECK(spots.row(150)[150]==source.row(150)[150]); CHECK(spots.row(150)[450]==source.row(150)[450]);
    CHECK((spots.row(400)[700]&255)==static_cast<uint32_t>((source.row(400)[700]&255)*0.5f));
    editor.ImagePointer(WM_LBUTTONDOWN,test::OnTestPoint({500,100})); editor.ImagePointer(WM_LBUTTONUP,test::OnTestPoint({500,100}));
    HWND opacity=::GetDlgItem(editor.context_hwnd(),2602);
    ::SendMessageW(opacity,TBM_SETPOS,TRUE,25); ::SendMessageW(editor.context_hwnd(),WM_HSCROLL,TB_THUMBPOSITION,reinterpret_cast<LPARAM>(opacity));
    CHECK(editor.document().annotations()[0].style.opacity==25 && editor.document().annotations()[1].style.opacity==25);
    undo(); CHECK(editor.Composite().pixels==spots.pixels);
    tool(ToolId::Watermark); HWND watermark=::GetDlgItem(editor.context_hwnd(),2604);
    ::SetWindowTextW(watermark,L"Easnap review"); CHECK(editor.document().annotations().back().tool==ToolId::Watermark);
    capture(L"spotlight-watermark-export.png");
    for(auto appearance:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(appearance); Pump(180);
        Shot(editor.hwnd(),appearance==app::AppearanceTheme::Light ? L"editor-light.png":L"editor-dark.png");
        Shot(editor.context_hwnd(),appearance==app::AppearanceTheme::Light ? L"context-watermark-light.png":L"context-watermark-dark.png");
        tool(ToolId::Mosaic); option(2521); Pump(180);
        Shot(editor.context_hwnd(),appearance==app::AppearanceTheme::Light ? L"context-blur-light.png":L"context-blur-dark.png");
        tool(ToolId::Text); Pump(180);
        Shot(editor.context_hwnd(),appearance==app::AppearanceTheme::Light ? L"context-text-light.png":L"context-text-dark.png");
        tool(ToolId::Watermark);
    }
    const auto invariant=editor.Composite(); const auto revision=editor.revision();
    Click(editor.hwnd(),2407); CHECK(editor.Composite().pixels==invariant.pixels && editor.revision()==revision);
    for(UINT dpi:{96,120,144,192}) {
        RECT r{30,30,1000,180}; ::SendMessageW(editor.hwnd(),WM_DPICHANGED,MAKEWPARAM(dpi,dpi),reinterpret_cast<LPARAM>(&r));
        RECT main=Bounds(editor.hwnd()),context=Bounds(editor.context_hwnd()); const auto work=platform::MonitorForRect(main).work_area;
        CHECK(main.left>=work.left && main.right<=work.right && context.left>=work.left && context.right<=work.right);
        CHECK(context.top>=main.bottom);
        CHECK(editor.Composite().pixels==invariant.pixels);
    }
    editor.Destroy();
    CHECK(editor.Open(test::OnTestMonitor({50,50,700,500}),nullptr,nullptr));
    editor.SetFrameInteracting(true); CHECK(!::IsWindowVisible(editor.hwnd()) && !::IsWindowVisible(editor.context_hwnd()));
    editor.SetRegion({50,50,740,540},true); editor.SetFrameInteracting(false);
    CHECK(::IsWindowVisible(editor.hwnd()) && !editor.has_image()); editor.Destroy();
    std::printf("Annotation editing: document transactions, native text, transforms, effects, export, themes and DPI checked\n");
}

void Workspace() {
    capture::DesktopSnapshot snapshot;
    snapshot.virtual_bounds=platform::VirtualScreenBounds();
    const auto monitors=platform::EnumerateMonitors(); CHECK(!monitors.empty());
    if(monitors.empty()) { return; }
    for(const auto& monitor:monitors) {
        capture::MonitorFrame frame; frame.desktop_rect=monitor.bounds;
        frame.image.Reset(platform::RectWidth(monitor.bounds),platform::RectHeight(monitor.bounds));
        std::fill(frame.image.pixels.begin(),frame.image.pixels.end(),0xff314257);
        snapshot.monitors.push_back(std::move(frame));
    }
    int cancelled=0,completed=0;
    ui::OverlayController::Callbacks callbacks;
    callbacks.on_cancelled=[&]{++cancelled;}; callbacks.on_screenshot_complete=[&]{++completed;};
    callbacks.on_paint_image=[](HDC dc,const RECT& bounds,const RECT&) {
        ui::theme::FillSolid(dc,{0,0,bounds.right-bounds.left,bounds.bottom-bounds.top},RGB(49,66,87));
    };
    ui::OverlayController overlay(std::move(callbacks));
    const auto monitor=test::SelectedTestMonitor() ? *test::SelectedTestMonitor():monitors.front();const POINT pointer{monitor.bounds.left+200,monitor.bounds.top+200};
    CHECK(overlay.Show(snapshot,pointer));
    const HWND modes=::FindWindowW(L"EasnapCaptureModes",nullptr);
    CHECK(modes && ::GetDlgItem(modes,3000) && ::GetDlgItem(modes,3001));
    CHECK(ui::CanCaptureVisibleWindows());
    HWND shade=nullptr;
    while((shade=::FindWindowExW(nullptr,shade,L"EasnapOverlayChrome",L"Easnap outside shading")) && ::GetWindow(shade,GW_OWNER)!=overlay.owner_window()) {}
    CHECK(shade);
    HRGN shading=::CreateRectRgn(0,0,0,0); CHECK(::GetWindowRgn(shade,shading)==NULLREGION);
    RECT selected{monitor.bounds.left+100,monitor.bounds.top+100,monitor.bounds.left+500,monitor.bounds.top+400};
    overlay.selector().SetSelection(selected); overlay.RegionChanged();
    CHECK(::GetWindowRgn(shade,shading)!=ERROR && !::PtInRegion(shading,200,200) && ::PtInRegion(shading,50,50));
    ::DeleteObject(shading);
    overlay.selector().SetImplicitSelection(monitor.bounds); overlay.RegionChanged();
    const auto frame=overlay.selector().selection();
    Appearance(app::AppearanceTheme::Light);Shot(modes,L"capture-modes-light.png");
    Shot(::FindWindowW(L"EasnapCaptureMagnifier",nullptr),L"magnifier-light.png",true);
    Appearance(app::AppearanceTheme::Dark);Shot(modes,L"capture-modes-dark.png");
    Shot(::FindWindowW(L"EasnapCaptureMagnifier",nullptr),L"magnifier-dark.png",true);
    CHECK(Same(overlay.selector().selection(),frame) && overlay.workspace().magnifier_visible());
    {
        ui::PopupScope popup(::GetDlgItem(modes,3000));
        ::SendMessageW(overlay.owner_window(),WM_KEYDOWN,VK_ESCAPE,0);
        ::SendMessageW(overlay.owner_window(),WM_KEYDOWN,VK_RETURN,0);
        ::SendMessageW(overlay.owner_window(),WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(150,150));
        ::SendMessageW(overlay.owner_window(),WM_LBUTTONUP,0,MAKELPARAM(150,150));
        CHECK(cancelled==0 && completed==0 && !overlay.workspace().editing());
        CHECK(Same(overlay.selector().selection(),frame));
    }
    Click(modes,3001);CHECK(overlay.mode()==app::CaptureMode::Record);
    Click(modes,3000);CHECK(overlay.mode()==app::CaptureMode::Screenshot);
    overlay.EnterEditing();
    CHECK(overlay.workspace().editing());
    {
        app::AppSettings settings;
        editor::ScreenshotEditor editor({});
        CHECK(editor.Open(selected,&settings,overlay.panel_owner_window()));
        const auto above_chrome=[&] {
            for(HWND window=editor.hwnd();window;window=::GetWindow(window,GW_HWNDNEXT)) {
                if(window==overlay.panel_owner_window()) { return true; }
            }
            return false;
        };
        for(HWND activated : {modes,overlay.owner_window()}) {
            ::SetWindowPos(activated,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
            ::SetActiveWindow(activated);
            CHECK(above_chrome());
        }
        editor.Destroy();
    }
    Appearance(app::AppearanceTheme::Light);
    overlay.FixScreenshot();
    CHECK(overlay.workspace().screenshot_fixed() && !::IsWindowEnabled(::GetDlgItem(modes,3001)));
    HWND image_window=::FindWindowW(L"EasnapCaptureImage",nullptr); CHECK(image_window);
    if(image_window) {
        // Observe the real compositor before any WM_PAINT is pumped. Only this
        // deterministic fixture surface is made capturable for the assertion.
        CHECK(::SetWindowDisplayAffinity(image_window,0)); ::DwmFlush();
        const auto image_bounds=Bounds(image_window); HDC screen=::GetDC(nullptr);
        const auto first_pixel=::GetPixel(screen,image_bounds.left+80,image_bounds.top+180);
        std::printf("First fixed frame pixel: %06lx\n",first_pixel);
        CHECK(first_pixel==RGB(49,66,87));
        ::ReleaseDC(nullptr,screen); CHECK(::SetWindowDisplayAffinity(image_window,0x11));
    }
    Appearance(app::AppearanceTheme::Dark); Pump(560);
    CHECK((::GetWindowLongPtrW(modes,GWL_STYLE)&WS_VISIBLE)==0);
    Appearance(app::AppearanceTheme::Light);
    CHECK((::GetWindowLongPtrW(modes,GWL_STYLE)&WS_VISIBLE)==0);
    CHECK(cancelled==0 && completed==0);
    overlay.Destroy();
    // Exercise the real capture provider while the excluded overlay remains
    // visible. A real solid HWND supplies safe, deterministic desktop pixels.
    WNDCLASSW fixture{}; fixture.hInstance=::GetModuleHandleW(nullptr); fixture.lpszClassName=L"EasnapCapturePixelFixture";
    fixture.lpfnWndProc=[](HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam)->LRESULT {
        if(message==WM_PAINT) {
            PAINTSTRUCT ps{}; HDC dc=::BeginPaint(hwnd,&ps); RECT r{};::GetClientRect(hwnd,&r);
            ui::theme::FillSolid(dc,r,static_cast<COLORREF>(::GetWindowLongPtrW(hwnd,GWLP_USERDATA)));::EndPaint(hwnd,&ps);return 0;
        }
        return ::DefWindowProcW(hwnd,message,wparam,lparam);
    };
    CHECK(::RegisterClassW(&fixture));
    RECT area{monitor.bounds.left+100,monitor.bounds.top+100,monitor.bounds.left+600,monitor.bounds.top+450};
    HWND backdrop=::CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW,fixture.lpszClassName,L"Capture test background",WS_POPUP,
        area.left,area.top,area.right-area.left,area.bottom-area.top,nullptr,nullptr,fixture.hInstance,nullptr); CHECK(backdrop);
    ::SetWindowLongPtrW(backdrop,GWLP_USERDATA,RGB(64,128,192));::ShowWindow(backdrop,SW_SHOWNOACTIVATE);::UpdateWindow(backdrop);Pump(50);
    auto provider=capture::CreateRegionCaptureProvider();
    CHECK(overlay.Show(snapshot,pointer)); overlay.selector().SetSelection(area);overlay.RegionChanged();overlay.EnterEditing();Pump(40);
    // Observe only the deterministic shade fixture. Expanding and shrinking
    // its mask must reuse the prepared black surface without a queued repaint.
    HWND live_shade=nullptr;
    while((live_shade=::FindWindowExW(nullptr,live_shade,L"EasnapOverlayChrome",L"Easnap outside shading")) &&
          ::GetWindow(live_shade,GW_OWNER)!=overlay.owner_window()) {}
    CHECK(live_shade && ::SetWindowDisplayAffinity(live_shade,0));
    const POINT sample{area.left+150,area.top+150};
    HDC desktop=::GetDC(nullptr);
    ::DwmFlush();
    const auto clear_pixel=::GetPixel(desktop,sample.x,sample.y);
    CHECK(clear_pixel==RGB(64,128,192));
    RECT smaller=area; smaller.left+=300;
    for(int repeat=0;repeat<3;++repeat) {
        overlay.selector().SetSelection(smaller);overlay.RegionChanged();::GdiFlush();::DwmFlush();
        const auto shaded_pixel=::GetPixel(desktop,sample.x,sample.y);
        CHECK(shaded_pixel!=CLR_INVALID && GetBValue(shaded_pixel)>0 && GetBValue(shaded_pixel)<GetBValue(clear_pixel));
        overlay.selector().SetSelection(area);overlay.RegionChanged();::GdiFlush();::DwmFlush();
        CHECK(::GetPixel(desktop,sample.x,sample.y)==clear_pixel);
    }
    ::ReleaseDC(nullptr,desktop);CHECK(::SetWindowDisplayAffinity(live_shade,0x11));
    for(COLORREF color:{RGB(64,128,192),RGB(160,96,48)}) {
        ::SetWindowLongPtrW(backdrop,GWLP_USERDATA,color);::InvalidateRect(backdrop,nullptr,FALSE);::UpdateWindow(backdrop);Pump(50);
        platform::BgraImage captured;CHECK(provider->CaptureRegion(area,false,captured));
        const uint32_t expected=0xff000000u|(static_cast<uint32_t>(GetRValue(color))<<16)|(static_cast<uint32_t>(GetGValue(color))<<8)|GetBValue(color);
        CHECK(!captured.empty() && (captured.row(150)[150]|0xff000000u)==expected);
        CHECK(::IsWindowVisible(overlay.owner_window()) && overlay.workspace().editing());
    }
    provider->Release(); overlay.Destroy();::DestroyWindow(backdrop);::UnregisterClassW(fixture.lpszClassName,fixture.hInstance);
}

// A client on an MTA exercises the real WM_GETOBJECT/provider boundary while
// this thread pumps the STA. No private member access or substitute provider.
bool ProbeTimeline(HWND window, bool endpoint=false) {
    platform::ComPtr<IUIAutomation> automation;
    if (FAILED(::CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))) { return false; }
    platform::ComPtr<IUIAutomationElement> root;
    if (FAILED(automation->ElementFromHandle(window,&root))) { return false; }
    platform::ComPtr<IUIAutomationCondition> condition;
    if (FAILED(automation->CreateTrueCondition(&condition))) { return false; }
    platform::ComPtr<IUIAutomationElementArray> children;
    if (FAILED(root->FindAll(TreeScope_Children,condition.Get(),&children))) { return false; }
    int count=0; children->get_Length(&count);
    if(count!=3) { std::printf("UIA timeline child count: %d\n",count); return false; }
    const wchar_t* expected[]{L"In point",L"Playback position",L"Out point"};
    for(int i=endpoint?1:0;i!=(endpoint?2:3);++i) {
        platform::ComPtr<IUIAutomationElement> element; children->GetElement(i,&element);
        BSTR name{}; element->get_CurrentName(&name); const bool correct=name && ::lstrcmpW(name,expected[i])==0; ::SysFreeString(name);
        if(!correct) { return false; }
        platform::ComPtr<IUIAutomationRangeValuePattern> pattern;
        if(FAILED(element->GetCurrentPatternAs(UIA_RangeValuePatternId,IID_PPV_ARGS(&pattern)))) { return false; }
        double min=0,max=0,value=0; pattern->get_CurrentMinimum(&min);pattern->get_CurrentMaximum(&max);pattern->get_CurrentValue(&value);
        if(value<min || value>max || !std::isfinite(max)) { return false; }
        if(SUCCEEDED(pattern->SetValue(max+1))) { return false; }
        const double requested=endpoint?max:(i==0?1.0:(i==1?2.0:8.0));
        if(FAILED(pattern->SetValue(requested))) { return false; }
        if(FAILED(pattern->get_CurrentValue(&value)) || std::abs(value-requested)>.01) { return false; }
        if(FAILED(element->SetFocus())) { return false; }
    }
    return true;
}
void ResizeSurfaceStability(ui::TrimWindow& trim) {
    const HWND window=trim.hwnd();
    const RECT original=Bounds(window);
    const auto appearance=ui::theme::Preference();
    const auto monitor=platform::MonitorForRect(original);
    const float scale=::GetDpiForWindow(window)/96.0f;
    POINT original_cursor{}; const bool restore_cursor=::GetCursorPos(&original_cursor)!=FALSE;
    ::SetWindowPos(window,nullptr,monitor.work_area.left,monitor.work_area.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
    // The adjacent erase/print samples must observe the same material state.
    // Keep the pointer over neutral chrome at every size, and let the native
    // button hover/enabled transitions finish before the exact comparison.
    CHECK(::SetCursorPos(monitor.work_area.left+ui::theme::Scaled(20,scale),monitor.work_area.top+ui::theme::Scaled(20,scale)));
    Pump(200);
    // Check the owner painter contract independently of desktop presentation:
    // USER32 may request erase without dispatching another paint during sizing.
    // Reading a child GetDC observes a composited backbuffer, not the display.
    // ComposedResizeStress below observes the actual display during native drag.
    const auto inspect=[&](const char* phase,bool settled) {
        struct Context { const char* phase; bool settled; } context{phase,settled};
        ::EnumChildWindows(window,[](HWND child,LPARAM parameter)->BOOL {
            auto& context=*reinterpret_cast<Context*>(parameter);
            // Foreign GPU preview pixels belong to the asynchronous helper.
            if(::GetWindowThreadProcessId(child,nullptr)!=::GetCurrentThreadId() || !::IsWindowVisible(child)) { return TRUE; }
            wchar_t name[64]{}; ::GetClassNameW(child,name,ARRAYSIZE(name));
            if(::lstrcmpW(name,L"EasnapTrimVideo")==0) { return TRUE; }
            RECT client{}; ::GetClientRect(child,&client);
            if(client.right<=0 || client.bottom<=0) { return TRUE; }
            platform::BgraImage erased; erased.Reset(client.right,client.bottom);
            std::fill(erased.pixels.begin(),erased.pixels.end(),0xff0df171);
            {
                platform::GdiImageSurface surface(erased); CHECK(surface.valid());
                CHECK(::SendMessageW(child,WM_ERASEBKGND,reinterpret_cast<WPARAM>(surface.dc()),0)!=0);
            }
            for(auto& pixel:erased.pixels) { pixel|=0xff000000u; }
            const auto expected=RenderControl(child);
            std::vector<RECT> descendants;
            for(HWND nested=::GetWindow(child,GW_CHILD);nested;nested=::GetWindow(nested,GW_HWNDNEXT)) {
                if(!::IsWindowVisible(nested)) { continue; }
                RECT box=Bounds(nested); ::MapWindowPoints(nullptr,child,reinterpret_cast<POINT*>(&box),2);
                descendants.push_back(box);
            }
            size_t differences=0;
            POINT first_difference{};
            for(int y=0;y<client.bottom;++y) for(int x=0;x<client.right;++x) {
                // A parent painter owns only pixels outside visible children.
                const POINT point{x,y};
                if(std::any_of(descendants.begin(),descendants.end(),[&](const RECT& box) { return ::PtInRect(&box,point)!=FALSE; })) { continue; }
                if(erased.row(y)[x]!=expected.row(y)[x]) {
                    if(!differences) { first_difference={x,y}; } ++differences;
                }
            }
            if(differences) {
                RECT update{}; const bool damaged=::GetUpdateRect(child,&update,FALSE)!=FALSE;
                std::printf("Trim child surface: phase=%s settled=%d class=%ls id=%d size=%ldx%ld differences=%zu visible=%d damaged=%d update=%ld,%ld,%ld,%ld\n",
                    context.phase,context.settled,name,::GetDlgCtrlID(child),client.right,client.bottom,differences,
                    ::IsWindowVisible(child),damaged,update.left,update.top,update.right,update.bottom);
                std::printf("Trim child erase/print difference: x=%ld y=%ld erased=%08x expected=%08x\n",first_difference.x,first_difference.y,
                    erased.row(first_difference.y)[first_difference.x],expected.row(first_difference.y)[first_difference.x]);
            }
            CHECK(differences==0);
            return TRUE;
        },reinterpret_cast<LPARAM>(&context));
    };
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(theme); Pump(80);
        ::SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
        for(const SIZE size:{SIZE{900,650},SIZE{640,430},SIZE{780,400},SIZE{480,700},SIZE{480,400}}) {
            const int width=std::min(ui::theme::Scaled(static_cast<float>(size.cx),scale),static_cast<int>(monitor.work_area.right-monitor.work_area.left));
            const int height=std::min(ui::theme::Scaled(static_cast<float>(size.cy),scale),static_cast<int>(monitor.work_area.bottom-monitor.work_area.top));
            ::SetWindowPos(window,nullptr,monitor.work_area.left,monitor.work_area.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
            inspect("native resize",false);
            Pump(30); inspect("native resize",true);
        }
        ::SendMessageW(window,WM_EXITSIZEMOVE,0,0);
        trim.SetStatus(L"A warning changes the current layout and exposes pixels previously owned by the preview and timeline.");
        Pump(30); inspect("warning appears",true);
        trim.SetStatus(L""); Pump(30); inspect("warning clears",true);
        trim.SetExporting(true); trim.SetExportProgress(.42f);
        const HWND progress=::FindWindowExW(window,nullptr,PROGRESS_CLASSW,nullptr);
        CHECK(WaitFor([&] { return ::IsWindowVisible(progress)!=FALSE; }));
        Pump(200);
        inspect("export appears",true);
        trim.SetExporting(false); trim.SetStatus(L""); Pump(200);
    }
    ::SetWindowPos(window,nullptr,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOZORDER|SWP_NOACTIVATE);
    Appearance(appearance); Pump(30);
    if(restore_cursor) { CHECK(::SetCursorPos(original_cursor.x,original_cursor.y)); }
}

void ComposedResizeStress(ui::TrimWindow& trim) {
    const HWND window=trim.hwnd();
    const RECT original=Bounds(window);
    const auto original_theme=ui::theme::Preference();
    const auto monitor=platform::MonitorForRect(original);
    const float scale=::GetDpiForWindow(window)/96.0f;
    const int min_width=ui::theme::Scaled(480,scale),min_height=ui::theme::Scaled(400,scale);
    const int width=std::min(ui::theme::Scaled(1100,scale),static_cast<int>(monitor.work_area.right-monitor.work_area.left));
    const int height=std::min(ui::theme::Scaled(740,scale),static_cast<int>(monitor.work_area.bottom-monitor.work_area.top));
    const RECT capture_area{monitor.work_area.left,monitor.work_area.top,monitor.work_area.left+width,monitor.work_area.top+height};
    if(width<min_width || height<min_height) { std::puts("Composed resize: monitor smaller than stress fixture"); CHECK(false); return; }
    // A test-owned background makes actual desktop exposure distinguishable
    // from video pixels without retaining any unrelated desktop content.
    constexpr COLORREF backdrop_color=RGB(13,241,113);
    constexpr uint32_t backdrop_pixel=0xff0df171;
    HBRUSH brush=::CreateSolidBrush(backdrop_color);
    WNDCLASSW definition{}; definition.lpfnWndProc=::DefWindowProcW;
    definition.hInstance=::GetModuleHandleW(nullptr); definition.hbrBackground=brush;
    definition.lpszClassName=L"EasnapComposedResizeBackdrop";
    CHECK(::RegisterClassW(&definition)!=0);
    HWND backdrop=::CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,definition.lpszClassName,L"Resize test background",WS_POPUP|WS_VISIBLE,
        capture_area.left,capture_area.top,width,height,nullptr,nullptr,definition.hInstance,nullptr);
    CHECK(backdrop!=nullptr);
    struct Probe {
        std::atomic<bool> sizing{false};
        std::atomic<unsigned> sizing_messages{0};
        std::atomic<ULONGLONG> max_dispatch_ms{0};
        HWND video{};
        std::atomic<unsigned> geometry_version{0};
        std::atomic<LONG> client_width{0},client_height{0},video_bottom{0},client_x{0},client_y{0};
        struct Geometry { unsigned version; LONG width,height,video_bottom,x,y; };
        Geometry Snapshot() const {
            Geometry result{geometry_version.load(),client_width.load(),client_height.load(),video_bottom.load(),client_x.load(),client_y.load()};
            if(result.version!=geometry_version.load()) { result.version|=1; }
            return result;
        }
        static LRESULT CALLBACK Proc(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR reference) {
            auto& self=*reinterpret_cast<Probe*>(reference);
            if(message==WM_ENTERSIZEMOVE) { self.sizing=true; }
            if(message==WM_EXITSIZEMOVE) { self.sizing=false; }
            if(message==WM_SIZING) { ++self.sizing_messages; }
            if(message==WM_WINDOWPOSCHANGED) { ++self.geometry_version; }
            const auto start=::GetTickCount64();
            const auto result=::DefSubclassProc(hwnd,message,wparam,lparam);
            if(message==WM_WINDOWPOSCHANGED) {
                RECT client{},video{}; ::GetClientRect(hwnd,&client);
                POINT origin{}; ::ClientToScreen(hwnd,&origin);
                if(self.video) { ::GetWindowRect(self.video,&video); ::MapWindowPoints(nullptr,hwnd,reinterpret_cast<POINT*>(&video),2); }
                self.client_width=client.right; self.client_height=client.bottom; self.video_bottom=video.bottom;
                self.client_x=origin.x; self.client_y=origin.y;
                ++self.geometry_version;
                const auto elapsed=::GetTickCount64()-start;
                auto previous=self.max_dispatch_ms.load();
                while(previous<elapsed && !self.max_dispatch_ms.compare_exchange_weak(previous,elapsed)) {}
            }
            return result;
        }
    } probe;
    CHECK(::SetWindowSubclass(window,Probe::Proc,0xEA6E,reinterpret_cast<DWORD_PTR>(&probe)));
    const HWND timeline=::FindWindowExW(window,nullptr,L"EasnapTrimTimeline",nullptr);
    const HWND play=::GetDlgItem(window,100);
    const HWND video=::FindWindowExW(window,nullptr,L"EasnapTrimVideo",nullptr);
    probe.video=video;
    const HWND presenter=::GetWindow(video,GW_CHILD);
    DWORD preview_process=0; ::GetWindowThreadProcessId(presenter,&preview_process);
    std::printf("Composed resize: test PID=%lu preview PID=%lu dpi=%u\n",::GetCurrentProcessId(),preview_process,::GetDpiForWindow(window));
    CHECK(preview_process!=0 && preview_process!=::GetCurrentProcessId());
    for(auto theme:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) for(bool exporting:{false,true}) {
        const std::wstring label=(theme==app::AppearanceTheme::Light ? L"light-" : L"dark-")+std::wstring(exporting ? L"export" : L"playing");
        Appearance(theme); trim.SetExporting(true); trim.SetExportResult(L"");
        ::SendMessageW(timeline,WM_KEYDOWN,VK_HOME,0);
        if(exporting) { trim.SetExporting(true); trim.SetExportProgress(.42f); }
        else {
            ::SendMessageW(play,BM_CLICK,0,0);
            CHECK(WaitFor([&] { return trim.range().current()>trim.range().in()+media::MediaTime{2'000'000}; },3000));
        }
        ::SetWindowPos(window,nullptr,capture_area.left,capture_area.top,width,height,SWP_NOZORDER|SWP_NOACTIVATE);
        ::SetWindowPos(backdrop,window,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        ::BringWindowToTop(window); ::SetForegroundWindow(window); Pump(200);
        const POINT start{capture_area.left+width-2,capture_area.top+height-2};
        const bool owned=::GetForegroundWindow()==window && ::GetAncestor(::WindowFromPoint(start),GA_ROOT)==window &&
            ::SendMessageW(window,WM_NCHITTEST,0,MAKELPARAM(start.x,start.y))==HTBOTTOMRIGHT;
        CHECK(owned); if(!owned) { break; }
        POINT previous_cursor{}; const bool restore_cursor=::GetCursorPos(&previous_cursor)!=FALSE;
        CHECK(::SetCursorPos(start.x,start.y)); Pump(20);
        std::atomic<bool> stop{false},ready{false},observe{false},input_done{false},input_ok{false};
        unsigned frames=0,holes=0,title_changes=0,capture_errors=0,fallbacks=0,timeouts=0,edge_samples=0,edge_growth_samples=0,black_edges=0;
        capture::RegionCaptureFailureDiagnostic first_fallback;
        ULONGLONG max_response_ms=0,max_frame_gap_ms=0;
        std::vector<std::pair<std::wstring,std::shared_ptr<const platform::BgraImage>>> pictures;
        const auto run_start=::GetTickCount64();
        probe.max_dispatch_ms=0; const auto sizing_before=probe.sizing_messages.load();
        std::thread health([&] {
            while(!stop) {
                DWORD_PTR result=0; const auto before=::GetTickCount64();
                const bool answered=::SendMessageTimeoutW(window,WM_NULL,0,0,SMTO_ABORTIFHUNG|SMTO_BLOCK,100,&result)!=0;
                max_response_ms=std::max(max_response_ms,::GetTickCount64()-before);
                if(!answered) { ++timeouts; }
                if(::GetTickCount64()-run_start>20'000) {
                    std::printf("FAIL: composed resize exceeded20s; terminating only test PID%lu\n",::GetCurrentProcessId());
                    std::fflush(stdout); ::TerminateProcess(::GetCurrentProcess(),124);
                }
                ::Sleep(20);
            }
        });
        std::thread observer([&] {
            ::CoInitializeEx(nullptr,COINIT_MULTITHREADED);
            auto provider=capture::CreateRegionCaptureProvider();
            std::shared_ptr<const platform::BgraImage> reference;
            Probe::Geometry previous_geometry{};
            ULONGLONG last_frame=0;
            const RECT title{ui::theme::Scaled(20,scale),ui::theme::Scaled(8,scale),ui::theme::Scaled(220,scale),ui::theme::Scaled(32,scale)};
            const int inset=ui::theme::Scaled(12,scale);
            const auto panel=ui::theme::Current().panel;
            const uint32_t panel_pixel=0xff000000u|(GetRValue(panel)<<16)|(GetGValue(panel)<<8)|GetBValue(panel);
            while(!stop) {
                const auto geometry=probe.Snapshot();
                media::VideoFrame frame;
                const auto result=provider->CaptureRegionFrame(capture_area,false,media::MediaTime{0},media::MediaTime{333'333},frame);
                if(result.status==capture::RegionCaptureStatus::Failed) { ++capture_errors; ::Sleep(5); continue; }
                if(result.status!=capture::RegionCaptureStatus::FrameAcquired || !frame.image) { ::Sleep(2); continue; }
                if(observe && result.primary_failure.stage!=capture::RegionCaptureFailureStage::None) {
                    if(!fallbacks) { first_fallback=result.primary_failure; }
                    ++fallbacks;
                }
                const auto now=::GetTickCount64();
                if(last_frame) { max_frame_gap_ms=std::max(max_frame_gap_ms,now-last_frame); } last_frame=now;
                size_t exposed=0,differences=0,title_ink=0;
                bool black_edge=false;
                // The right 12-DIP gutter below the preview belongs entirely
                // to the panel. Observe growth beyond the invariant minimum
                // rectangle without mistaking video letterboxing for damage.
                // Skip captures concurrent with layout, and exclude both the
                // current and preceding preview extents for compositor lag.
                if(!(geometry.version&1) && geometry.version==probe.geometry_version.load()) {
                    const int left=geometry.x-capture_area.left+geometry.width-ui::theme::Scaled(9,scale);
                    const int right=geometry.x-capture_area.left+geometry.width-ui::theme::Scaled(5,scale);
                    const int top=std::max(geometry.y+geometry.video_bottom,previous_geometry.y+previous_geometry.video_bottom)-capture_area.top+ui::theme::Scaled(4,scale);
                    const int bottom=std::min(geometry.y+geometry.height,previous_geometry.y+previous_geometry.height)-capture_area.top-inset;
                    const int required=ui::theme::Scaled(24,scale);
                    if(observe && left>=0 && right<frame.image->width && top>=0 && bottom<=frame.image->height && bottom-top>=required) {
                        ++edge_samples;
                        if(probe.sizing && geometry.width>min_width+ui::theme::Scaled(64,scale)) { ++edge_growth_samples; }
                        int consecutive=0;
                        for(int y=top;y<bottom && !black_edge;++y) {
                            bool black=true;
                            for(int x=left;x<right;++x) { if(frame.image->row(y)[x]!=0xff000000u) { black=false; break; } }
                            consecutive=black ? consecutive+1 : 0;
                            black_edge=consecutive>=required;
                        }
                    }
                    previous_geometry=geometry;
                }
                for(int y=inset;y<min_height-inset;y+=2) for(int x=inset;x<min_width-inset;x+=2) {
                    if(frame.image->row(y)[x]==backdrop_pixel) { ++exposed; }
                }
                for(int y=title.top;y<title.bottom;++y) for(int x=title.left;x<title.right;++x) {
                    const auto pixel=frame.image->row(y)[x];
                    if(pixel!=panel_pixel && pixel!=backdrop_pixel) { ++title_ink; }
                    if(reference && pixel!=reference->row(y)[x]) { ++differences; }
                }
                if(!ready) {
                    // A paused preview need not produce subsequent desktop
                    // frames. Accept the already composed, visibly titled UI.
                    if(exposed<16 && title_ink>=20) { reference=frame.image; pictures.emplace_back(L"reference",reference); ready=true; }
                } else if(observe) {
                    ++frames;
                    if(exposed>=16) { ++holes; }
                    if(differences) { ++title_changes; }
                    if(black_edge) { ++black_edges; }
                    if((exposed>=16 || differences || black_edge) && pictures.size()<7) {
                        pictures.emplace_back(L"frame-"+std::to_wstring(frames)+L"-exposed-"+std::to_wstring(exposed)+L"-title-"+std::to_wstring(differences)+L"-black-edge-"+std::to_wstring(black_edge),frame.image);
                    }
                }
            }
            provider->Release(); ::CoUninitialize();
        });
        const bool observer_ready=WaitFor([&] { return ready.load(); },3000); CHECK(observer_ready);
        if(observer_ready) {
            observe=true;
            std::thread input([&] {
                INPUT mouse{}; mouse.type=INPUT_MOUSE; mouse.mi.dwFlags=MOUSEEVENTF_LEFTDOWN;
                bool valid=::SetCursorPos(start.x,start.y)!=FALSE;
                valid=(::SendInput(1,&mouse,sizeof(mouse))==1)&&valid;
                const auto enter_deadline=::GetTickCount64()+1000;
                while(valid && !probe.sizing && ::GetTickCount64()<enter_deadline && ::GetForegroundWindow()==window) { ::Sleep(5); }
                valid=valid && probe.sizing;
                const auto end=::GetTickCount64()+6000;
                while(valid && ::GetTickCount64()<end && probe.sizing && ::GetForegroundWindow()==window) {
                    const auto phase=static_cast<int>((::GetTickCount64()-run_start)%320);
                    const int horizontal=phase<160 ? phase : 320-phase;
                    const int shifted=(phase+80)%320,vertical=shifted<160 ? shifted : 320-shifted;
                    const int w=width-horizontal*(width-min_width)/160,h=height-vertical*(height-min_height)/160;
                    valid=::SetCursorPos(capture_area.left+w-2,capture_area.top+h-2)!=FALSE;
                    ::Sleep(5);
                }
                // Keep the native drag active at the expanded extent long
                // enough to sample its panel gutter even in slower Debug runs.
                // The six seconds of rapid oscillation above remain unchanged.
                if(valid && probe.sizing && ::GetForegroundWindow()==window) {
                    valid=::SetCursorPos(start.x,start.y)!=FALSE;
                    ::Sleep(300);
                }
                mouse.mi.dwFlags=MOUSEEVENTF_LEFTUP; valid=(::SendInput(1,&mouse,sizeof(mouse))==1)&&valid;
                ::Sleep(40); if(probe.sizing) { ::PostMessageW(window,WM_CANCELMODE,0,0); }
                input_ok=valid; input_done=true;
            });
            CHECK(WaitFor([&] { return input_done.load(); },9000));
            input.join(); CHECK(input_ok && !probe.sizing);
        }
        stop=true; observer.join(); health.join();
        if(restore_cursor && ::GetForegroundWindow()==window) { ::SetCursorPos(previous_cursor.x,previous_cursor.y); }
        std::printf("Composed resize %ls: frames=%u holes=%u title_changes=%u capture_errors=%u fallbacks=%u max_frame_gap_ms=%llu response_timeouts=%u max_response_ms=%llu max_windowpos_ms=%llu sizing=%u\n",
            label.c_str(),frames,holes,title_changes,capture_errors,fallbacks,max_frame_gap_ms,timeouts,max_response_ms,probe.max_dispatch_ms.load(),probe.sizing_messages.load()-sizing_before);
        std::printf("Composed capture backend %ls: DXGI=%u desktop_GDI=%u fallback_stage=%u code_kind=%u native_code=0x%08x\n",
            label.c_str(),frames-fallbacks,fallbacks,static_cast<unsigned>(first_fallback.stage),static_cast<unsigned>(first_fallback.code_kind),first_fallback.native_code);
        std::printf("Composed right panel gutter %ls: samples=%u beyond_minimum_during_drag=%u black_stripes=%u\n",label.c_str(),edge_samples,edge_growth_samples,black_edges);
        if(!evidence.empty()) for(const auto& picture:pictures) {
            CHECK(platform::SaveImage(*picture.second,(evidence/(L"trim-composed-"+label+L"-"+picture.first+L".png")).wstring()));
        }
        CHECK(frames>=20 && capture_errors==0);
        CHECK(holes==0 && title_changes==0);
        CHECK(edge_growth_samples>0 && black_edges==0);
        CHECK(probe.sizing_messages>sizing_before && probe.max_dispatch_ms<1000);
        trim.SetExporting(true); trim.SetExportResult(L"");
    }
    ::RemoveWindowSubclass(window,Probe::Proc,0xEA6E);
    ::DestroyWindow(backdrop); ::UnregisterClassW(definition.lpszClassName,definition.hInstance); ::DeleteObject(brush);
    ::SetWindowPos(window,nullptr,original.left,original.top,original.right-original.left,original.bottom-original.top,SWP_NOZORDER|SWP_NOACTIVATE);
    Appearance(original_theme); Pump(30);
}

#include "trim_resize_presentation.h"
#include "trim_endpoint_presentation.h"
#include "trim_seek_stress.h"

void Trim(bool all_edges_only=false, const wchar_t* edge_filter=nullptr, int layout_only=0, bool paused_only=false, bool external_observation=false, bool external_capture=false, bool control_states=false) {
    std::puts("Trim: encoding real H.264 fixture (audio has separate integration gates)");
    const auto source=std::filesystem::temp_directory_path()/(L"easnap_ui_"+std::to_wstring(::GetCurrentProcessId())+L".mp4");
    CHECK(test::CreateVideoPreviewSource(source.wstring()));
    std::puts("Trim: opening production preview");
    int saves=0,copies=0,discards=0,cancels=0;
    ui::TrimWindow::Callbacks callbacks;
    callbacks.on_save=[&](media::MediaTime,media::MediaTime){++saves;};
    callbacks.on_copy=[&](media::MediaTime,media::MediaTime){++copies;};
    callbacks.on_discard_requested=[&]{++discards;};
    callbacks.on_cancel_export=[&]{++cancels;};
    ui::TrimWindow trim(std::move(callbacks));
    const auto preview_source=layout_only==2 ? source.wstring()+L".missing" : source.wstring();
    if(layout_only==2) CHECK(!std::filesystem::exists(preview_source));
    CHECK(trim.Open(preview_source,test::OnTestMonitor({100,100,740,460})));
    std::puts("Trim: native window open; waiting for preview readiness");
    const auto until=::GetTickCount64()+15'000;
    while(!layout_only && !::IsWindowEnabled(::GetDlgItem(trim.hwnd(),IDOK)) && ::GetTickCount64()<until) { Pump(20); }
    if(layout_only==1) CHECK(WaitFor([&]{return trim.range().valid();},15000));
    if(layout_only!=2) CHECK(trim.range().valid());
    if(external_observation) {
        // A separate desktop recorder/input driver can observe production UI
        // with no in-process capture, geometry probe or pixel comparator.
        // This is a diagnostic fixture, never a visual gate pass.
        std::printf("External observation fixture: pid=%lu; 60 seconds; visual result unverified\n",::GetCurrentProcessId());
        Pump(60'000);
        trim.Destroy();Pump(100);std::filesystem::remove(source);return;
    }
    if(all_edges_only) {
        if(control_states) {
            // Exercise the real view's public status/progress transitions.
            // Export backend correctness is covered separately; this gate
            // validates the controls visible while that work is in progress.
            trim.SetStatus(L"The original recording remains available.");
            AllEdgeResizePresentation(trim,L"status",edge_filter);
            trim.SetStatus(L"");trim.SetExporting(true);trim.SetExportProgress(.42f);
            const HWND progress=::FindWindowExW(trim.hwnd(),nullptr,PROGRESS_CLASSW,nullptr);
            CHECK(WaitFor([&]{return ::IsWindowVisible(progress)!=FALSE;},1000));
            AllEdgeResizePresentation(trim,L"progress",edge_filter);
            const HWND timeline=::FindWindowExW(trim.hwnd(),nullptr,L"EasnapTrimTimeline",nullptr);
            CHECK(!::IsWindowEnabled(timeline) && ::IsWindowEnabled(::GetDlgItem(trim.hwnd(),103)));
            Click(trim.hwnd(),103);CHECK(cancels==1);
            trim.SetExporting(true,true);
            CHECK(!::IsWindowEnabled(::GetDlgItem(trim.hwnd(),103)));
            trim.SetExporting(false);trim.SetStatus(L"");
            CHECK(::IsWindowEnabled(timeline));
            Click(trim.hwnd(),IDOK);Click(trim.hwnd(),104);Click(trim.hwnd(),IDCANCEL);
            CHECK(saves==1 && copies==1 && discards==1);
            trim.Destroy();Pump(100);std::filesystem::remove(source);return;
        }
        if(layout_only) {
            trim.ReleaseMedia();Pump(100);
            trim.SetStatus(L"");
            const HWND video=::FindWindowExW(trim.hwnd(),nullptr,L"EasnapTrimVideo",nullptr);
            CHECK(video && !::GetWindow(video,GW_CHILD));
            std::puts("Diagnostic isolation: preview released; this is not the media resize gate");
            AllEdgeResizePresentation(trim,layout_only==2 ? L"never-opened" : L"layout-only",edge_filter);
            trim.Destroy();Pump(100);std::filesystem::remove(source);return;
        }
        std::puts("All-edge presentation: native paused frame");
        AllEdgeResizePresentation(trim,L"paused",edge_filter,external_capture);
        if(paused_only) {
            std::puts("Diagnostic paused subset; endpoint and playback resize are not covered");
            trim.Destroy();Pump(100);std::filesystem::remove(source);return;
        }
        const HWND timeline=::FindWindowExW(trim.hwnd(),nullptr,L"EasnapTrimTimeline",nullptr);
        CHECK(timeline && AutomationClient([&]{return ProbeTimeline(timeline,true);}));
        CHECK(WaitFor([&]{return trim.range().current()==trim.range().out();},3000));
        // The public timeline seek reaches the real helper-owned endpoint still.
        // Observe live caption movement as well as all resize directions in
        // both presentation modes; never substitute a screenshot for preview.
        CHECK(WaitFor([&] {
            const HWND video=::FindWindowExW(trim.hwnd(),nullptr,L"EasnapTrimVideo",nullptr);
            const HWND host=::GetWindow(video,GW_CHILD);
            const HWND still=::FindWindowExW(host,nullptr,L"EasnapPreviewStillFrame",nullptr);
            const HWND native=::FindWindowExW(host,nullptr,L"EasnapPreviewNativeVideo",nullptr);
            return still && native && ::IsWindowVisible(still) && !::IsWindowVisible(native);
        },3000));
        std::puts("All-edge presentation: retained final frame");
        AllEdgeResizePresentation(trim,L"endpoint",edge_filter);
        std::puts("All-edge presentation: native playing frame");
        AllEdgeResizePresentation(trim,L"playing",edge_filter);
        trim.Destroy();Pump(100);std::filesystem::remove(source);return;
    }
    const HWND timeline=::FindWindowExW(trim.hwnd(),nullptr,L"EasnapTrimTimeline",nullptr);
    CHECK(timeline && ::IsWindowEnabled(timeline));
    if(timeline && ::IsWindowEnabled(timeline)) {
        std::puts("Trim: querying UI Automation and editing all three values");
        CHECK(AutomationClient([&]{return ProbeTimeline(timeline);}));
        CHECK(trim.range().in()==media::MediaTime{10'000'000}); CHECK(trim.range().out()==media::MediaTime{80'000'000});
    }
    const auto in=trim.range().in(),out=trim.range().out(),position=trim.range().current();
    ResizeSurfaceStability(trim);
    std::puts("Trim: verifying theme invariants, responsive layout and busy/error states");
    const auto native_window=trim.hwnd(); const auto original=Bounds(native_window);
    RECT client{}; ::GetClientRect(native_window,&client);
    POINT client_origin{};CHECK(::ClientToScreen(native_window,&client_origin));
    const int side_frame=client_origin.x-original.left;
    const int bottom_frame=original.bottom-client_origin.y-client.bottom;
    CHECK(side_frame>0 && bottom_frame>0 && client_origin.y==original.top);
    CHECK(client.right+2*side_frame==original.right-original.left);
    HWND chrome=::FindWindowExW(native_window,nullptr,L"EasnapWindowChrome",nullptr); CHECK(chrome);
    const HWND maximize=::FindWindowExW(chrome,nullptr,L"BUTTON",L"Maximize or restore");
    const HWND minimize=::FindWindowExW(chrome,nullptr,L"BUTTON",L"Minimize");
    const HWND close=::FindWindowExW(chrome,nullptr,L"BUTTON",L"Close");
    CHECK(maximize && minimize && close);
    CHECK(::SendMessageW(native_window,WM_NCHITTEST,0,MAKELPARAM(original.left+1,original.top+1))==HTTOPLEFT);
    CHECK(::SendMessageW(chrome,WM_NCHITTEST,0,MAKELPARAM(original.left+1,original.top+1))==HTTRANSPARENT);
    for(int i=0;i!=30;++i) {
        Appearance(i%2?app::AppearanceTheme::Light:app::AppearanceTheme::Dark);
        CHECK(trim.hwnd()==native_window && Same(Bounds(native_window),original));
        CHECK(trim.range().in()==in && trim.range().out()==out && trim.range().current()==position);
        CHECK(saves==0 && copies==0);
    }
    Shot(native_window,L"trim-light.png"); Appearance(app::AppearanceTheme::Dark);Shot(native_window,L"trim-dark.png");
    ::SetWindowPos(native_window,nullptr,0,0,440,420,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    Shot(native_window,L"trim-compact-dark.png");
    const HWND status=::FindWindowExW(native_window,nullptr,L"Edit",nullptr);
    const HWND progress=::FindWindowExW(native_window,nullptr,PROGRESS_CLASSW,nullptr);
    const HWND video=::FindWindowExW(native_window,nullptr,L"EasnapTrimVideo",nullptr);
    const HWND time_readout=::FindWindowExW(native_window,nullptr,L"Static",nullptr);
    const HWND range_readout=::FindWindowExW(native_window,time_readout,L"Static",nullptr);
    CHECK(time_readout && range_readout);
    const auto check_trim_text=[&] {
        const float scale=::GetDpiForWindow(native_window)/96.0f;
        const auto font=reinterpret_cast<HFONT>(::SendMessageW(time_readout,WM_GETFONT,0,0));
        HDC dc=::GetDC(time_readout); const auto old=::SelectObject(dc,font);
        TEXTMETRICW metrics{}; CHECK(::GetTextMetricsW(dc,&metrics));
        for(HWND readout:{time_readout,range_readout}) {
            RECT bounds{}; ::GetClientRect(readout,&bounds);
            wchar_t text[512]{}; ::GetWindowTextW(readout,text,512);
            RECT measured{0,0,bounds.right,0};
            ::DrawTextW(dc,text,-1,&measured,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
            CHECK(bounds.bottom>=measured.bottom && bounds.bottom>=metrics.tmHeight);
        }
        ::SelectObject(dc,old); ::ReleaseDC(time_readout,dc);
        // Compare complete glyph coverage to unclipped GDI text, independently
        // of child-rectangle containment. Both endpoint labels must fit.
        const auto painted=RenderControl(timeline);
        const auto color=ui::theme::Current().text;
        const uint32_t ink=0xff000000u|(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
        const int half=ui::theme::Scaled(18,scale);
        const int track_width=painted.width-ui::theme::Scaled(36,scale);
        for(const auto endpoint:{std::pair{L"In",trim.range().in()},std::pair{L"Out",trim.range().out()}}) {
            platform::BgraImage reference; reference.Reset(2*half,metrics.tmHeight);
            { platform::GdiImageSurface surface(reference); CHECK(surface.valid());
                RECT area{0,0,reference.width,reference.height};
                ui::theme::FillSolid(surface.dc(),area,ui::theme::Current().panel);
                const auto old_font=::SelectObject(surface.dc(),font);
                ::SetBkMode(surface.dc(),TRANSPARENT); ::SetTextColor(surface.dc(),color);
                ::DrawTextW(surface.dc(),endpoint.first,-1,&area,DT_CENTER|DT_BOTTOM|DT_SINGLELINE);
                ::SelectObject(surface.dc(),old_font); ::GdiFlush(); }
            size_t expected=0,actual=0;
            for(auto pixel:reference.pixels) { if((pixel|0xff000000u)==ink) { ++expected; } }
            const int left=static_cast<int>(trim.range().TimelineOffsetFromTime(endpoint.second,static_cast<uint32_t>(track_width)));
            for(int y=0;y<metrics.tmHeight && y<painted.height;++y) {
                for(int x=left;x<left+2*half && x<painted.width;++x) {
                    if(painted.row(y)[x]==ink) { ++actual; }
                }
            }
            CHECK(expected>0 && actual==expected);
        }
    };
    CHECK(!::FindWindowExW(native_window,nullptr,L"Static",L"Your recording"));
    CHECK(!::IsWindowVisible(status) && !::IsWindowVisible(progress));
    const auto quiet=Bounds(::GetDlgItem(native_window,100));
    const auto save_box=Bounds(::GetDlgItem(native_window,IDOK));
    CHECK(save_box.top-quiet.bottom<=ui::theme::Scaled(8,::GetDpiForWindow(native_window)/96.0f));
    trim.SetStatus(L"Short notice"); const auto short_notice=Bounds(status);
    trim.SetStatus(L"A longer message must wrap to fit this window and must reserve only the height it needs. The original recording remains available while you choose a new destination.");
    CHECK(Bounds(status).bottom-Bounds(status).top>short_notice.bottom-short_notice.top);
    trim.SetStatus(L""); CHECK(!::IsWindowVisible(status));
    ::SetWindowPos(native_window,nullptr,0,0,1200,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    const auto wide_video=Bounds(video);
    ::SetWindowPos(native_window,nullptr,0,0,900,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    const auto narrow_video=Bounds(video);
    CHECK(wide_video.right-wide_video.left==narrow_video.right-narrow_video.left);
    CHECK(wide_video.bottom-wide_video.top==narrow_video.bottom-narrow_video.top);
    for(int width:{400,700,1000,700,400}) {
        ::SetWindowPos(native_window,nullptr,0,0,width,500,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        const auto picture=Bounds(video);
        CHECK(std::abs((picture.right-picture.left)*9.0/16-(picture.bottom-picture.top))<=1);
    }
    // Native sizing can request background erasure before dispatching PAINT.
    // Both backing surfaces must actually supply theme pixels in that interval.
    const auto check_resize_backing=[&](HWND window) {
        RECT r{}; ::GetClientRect(window,&r);
        platform::BgraImage backing; backing.Reset(r.right,r.bottom);
        platform::GdiImageSurface surface(backing); CHECK(surface.valid());
        ::SendMessageW(window,WM_ERASEBKGND,reinterpret_cast<WPARAM>(surface.dc()),0);
        CHECK(::GetPixel(surface.dc(),r.right/2,r.bottom/2)==ui::theme::Current().panel);
    };
    // Inspect the owner painters after native resize, maximize and restore.
    // Actual desktop presentation is covered by ComposedResizeStress; a child
    // GetDC may expose its offscreen backbuffer under WS_EX_COMPOSITED.
    const auto check_resize_surface=[&] {
        Pump(30);
        RECT area{}; ::GetClientRect(native_window,&area);
        const auto panel=RenderControl(native_window);
        const auto color=ui::theme::Current().panel;
        const uint32_t panel_pixel=0xff000000u|(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
        const int gap=ui::theme::Scaled(6,::GetDpiForWindow(native_window)/96.0f);
        CHECK(panel.row(area.bottom-gap/2)[area.right/2]==panel_pixel);
        CHECK(panel.row(area.bottom-ui::theme::Scaled(24,::GetDpiForWindow(native_window)/96.0f))[gap/2]==panel_pixel);
        RECT header_area{}; ::GetClientRect(chrome,&header_area);
        const auto header=RenderControl(chrome);
        const int x=header_area.right-ui::theme::Scaled(43,::GetDpiForWindow(native_window)/96.0f);
        CHECK(header.row(header_area.bottom/2)[x]==panel_pixel);
        CHECK(header.row(header_area.bottom/2)[header_area.right-ui::theme::Scaled(83,::GetDpiForWindow(native_window)/96.0f)]==panel_pixel);
        check_resize_backing(native_window); check_resize_backing(chrome); check_resize_backing(video);
    };
    for(auto appearance:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(appearance);
        ::SendMessageW(native_window,WM_ENTERSIZEMOVE,0,0);
        for(int i=0;i<24;++i) {
            const int size=600+(i%2 ? 23-i:i)*16;
            ::SetWindowPos(native_window,nullptr,0,0,size,size-100,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            check_resize_surface();
        }
        ::SendMessageW(native_window,WM_EXITSIZEMOVE,0,0);
    }
    ::SendMessageW(maximize,BM_CLICK,0,0); CHECK(::IsZoomed(native_window)); check_resize_surface();
    RECT maximized_client{};CHECK(::GetClientRect(native_window,&maximized_client));
    ::MapWindowPoints(native_window,nullptr,reinterpret_cast<POINT*>(&maximized_client),2);
    CHECK(Same(maximized_client,platform::MonitorForRect(Bounds(native_window)).work_area));
    Shot(native_window,L"trim-maximized-dark.png");
    ::SendMessageW(maximize,BM_CLICK,0,0); CHECK(!::IsZoomed(native_window)); check_resize_surface();
    ::SendMessageW(minimize,BM_CLICK,0,0); CHECK(::IsIconic(native_window));
    ::ShowWindow(native_window,SW_RESTORE); check_resize_surface();
    MINMAXINFO minimum{}; ::SendMessageW(native_window,WM_GETMINMAXINFO,0,reinterpret_cast<LPARAM>(&minimum));
    const float dpi=::GetDpiForWindow(native_window)/96.0f;
    std::printf("Trim: native window DPI %u (%.0f%% display scaling)\n",::GetDpiForWindow(native_window),static_cast<double>(dpi)*100.0);
    CHECK(minimum.ptMinTrackSize.x-2*side_frame==ui::theme::Scaled(480,dpi));
    CHECK(minimum.ptMinTrackSize.y-bottom_frame==ui::theme::Scaled(400,dpi));
    std::puts("Trim: measured endpoint/readout glyphs and dynamic status budget");
    for(auto appearance:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(appearance);
        for(const auto size:{std::pair{480,400},std::pair{1200,400},std::pair{480,800}}) {
            ::SetWindowPos(native_window,nullptr,0,0,ui::theme::Scaled(static_cast<float>(size.first),dpi),ui::theme::Scaled(static_cast<float>(size.second),dpi),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            trim.SetExportResult(L""); check_trim_text();
            trim.SetExporting(true,true); trim.SetExportProgress(.42f); check_trim_text();
            std::wstring notice;
            for(int line=0;line<24;++line) { notice+=std::to_wstring(line)+L": The original recording remains available; choose a different destination and retry.\r\n"; }
            trim.SetStatus(notice,true); check_trim_text();
            CHECK(::IsWindowVisible(status) && ::IsWindowVisible(progress));
            CHECK(Bounds(status).bottom<=Bounds(progress).top);
            CHECK((::GetWindowLongPtrW(status,GWL_STYLE)&(WS_VSCROLL|WS_TABSTOP))==(WS_VSCROLL|WS_TABSTOP));
            const auto first=RenderControl(status);
            ::SendMessageW(status,EM_LINESCROLL,0,12);
            CHECK(::SendMessageW(status,EM_GETFIRSTVISIBLELINE,0,0)>0);
            CHECK(RenderControl(status).pixels!=first.pixels);
            // Collapse/re-expand must discard the old scroll offset as well as
            // the old progress bounds, without clipping either readout.
            trim.SetExportResult(L"Export cancelled. The source recording was kept.");
            CHECK(!trim.exporting() && !::IsWindowVisible(progress));
            CHECK(::IsWindowVisible(status) && ::SendMessageW(status,EM_GETFIRSTVISIBLELINE,0,0)==0);
            check_trim_text();
        }
    }
    trim.SetStatus(L"");
    trim.SetExporting(true); trim.SetExportProgress(.42f); trim.SetStatus(L"Exporting selected range..."); Pump(300);
    CHECK(WaitFor([&] { return ::IsWindowVisible(progress) && !::IsWindowVisible(status); }));
    CHECK(::SendMessageW(progress,PBM_GETPOS,0,0)==420);
    std::puts("Trim: export fill endpoints and one accent layer across resize");
    for(auto appearance:{app::AppearanceTheme::Light,app::AppearanceTheme::Dark}) {
        Appearance(appearance);
        for(int width:{480,900}) {
            ::SetWindowPos(native_window,nullptr,0,0,ui::theme::Scaled(static_cast<float>(width),dpi),ui::theme::Scaled(500,dpi),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            for(int value:{0,1,420,999,1000}) {
                trim.SetExportProgress(value/1000.0f);
                const auto fill=RenderControl(progress);
                const auto color=ui::theme::Current().accent;
                const uint32_t ink=0xff000000u|(GetRValue(color)<<16)|(GetGValue(color)<<8)|GetBValue(color);
                const int inset=ui::theme::Scaled(4,dpi)+ui::theme::Scaled(3,dpi);
                for(int y=0;y<fill.height;++y) for(int x=0;x<fill.width;++x) {
                    if(value==0 || x<inset || x>=fill.width-inset || y<inset || y>=fill.height-inset) {
                        CHECK(fill.row(y)[x]!=ink);
                    }
                }
                if(value>=420) {
                    const int x=inset+std::max(ui::theme::Scaled(10,dpi),fill.height-2*inset);
                    for(int y=inset+1;y<fill.height-inset-1;++y) { CHECK(fill.row(y)[x]==ink); }
                }
            }
        }
    }
    trim.SetExportProgress(.42f);
    Appearance(app::AppearanceTheme::Light);Shot(native_window,L"trim-exporting-light.png");
    CHECK(trim.exporting() && !::IsWindowEnabled(timeline));
    trim.SetExportResult(L"Could not save. The source recording is still available.",true);
    RECT status_format{}; ::SendMessageW(status,EM_GETRECT,0,reinterpret_cast<LPARAM>(&status_format));
    HDC status_dc=::GetDC(status); const auto old_status_font=::SelectObject(status_dc,reinterpret_cast<HFONT>(::SendMessageW(status,WM_GETFONT,0,0))); TEXTMETRICW status_metrics{}; ::GetTextMetricsW(status_dc,&status_metrics); ::SelectObject(status_dc,old_status_font); ::ReleaseDC(status,status_dc);
    CHECK(status_format.bottom-status_format.top>=status_metrics.tmHeight*::SendMessageW(status,EM_GETLINECOUNT,0,0));
    Shot(native_window,L"trim-error-light.png");
    for(int id:{100,101,102,IDOK,104,IDCANCEL}) { CHECK(::IsWindowVisible(::GetDlgItem(native_window,id))); }
    ::SendMessageW(close,BM_CLICK,0,0); CHECK(WaitFor([&] { return discards==1; }));
    CHECK(saves==0 && copies==0);
    ComposedResizeStress(trim);
    std::puts("Trim: hiding retires queued UI before real preview teardown");
    // The stress fixture ends with no status; establish a nonempty surface so
    // this lifecycle check can detect an obsolete callback repaint after Hide.
    trim.SetStatus(L"The source recording is still available.",true);
    const HWND presenter=::GetWindow(video,GW_CHILD); CHECK(presenter && ::IsWindow(presenter));
    trim.Hide();
    CHECK(!trim.visible() && !::IsWindowVisible(native_window) && ::IsWindow(presenter));
    const auto hidden_status=RenderControl(status); const auto hidden_bounds=Bounds(status);
    const auto hidden_range=trim.range().current();
    const auto hidden_in=trim.range().in(),hidden_out=trim.range().out();
    const HWND hidden_focus=::GetFocus(),hidden_capture=::GetCapture();
    trim.SetExporting(true); trim.SetExportProgress(.9f); trim.SetStatus(L"Obsolete status",true);
    trim.RefreshLayout(); trim.Activate();
    ::SendMessageW(native_window,WM_TIMER,1,0); ::SendMessageW(native_window,WM_APP+1,0,0);
    ::SendMessageW(timeline,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(90,20));
    ::SendMessageW(timeline,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(180,20));
    ::SendMessageW(timeline,WM_LBUTTONUP,0,MAKELPARAM(180,20));
    ::SendMessageW(timeline,WM_KEYDOWN,VK_HOME,0);
    ::SendMessageW(timeline,WM_KEYDOWN,VK_RIGHT,0);
    ::SendMessageW(video,WM_SIZE,SIZE_RESTORED,0); ::SendMessageW(video,WM_PAINT,0,0);
    CHECK(!trim.visible() && !::IsWindowVisible(native_window));
    CHECK(Same(Bounds(status),hidden_bounds) && RenderControl(status).pixels==hidden_status.pixels);
    CHECK(trim.range().current()==hidden_range && !trim.exporting());
    CHECK(trim.range().in()==hidden_in && trim.range().out()==hidden_out);
    CHECK(::GetFocus()==hidden_focus && ::GetCapture()==hidden_capture);
    trim.Destroy(); Pump(100);
    CHECK(!::IsWindow(presenter));
    std::error_code error;std::filesystem::remove(source,error);CHECK(!error);
}
}
int wmain(int argc,wchar_t** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ::CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES|ICC_PROGRESS_CLASS};::InitCommonControlsEx(&controls);
    if(argc==3 && (::lstrcmpW(argv[1],L"--evidence")==0 || ::lstrcmpW(argv[1],L"--editor-checks")==0 || ::lstrcmpW(argv[1],L"--trim-checks")==0 || ::lstrcmpW(argv[1],L"--text-checks")==0 || ::lstrcmpW(argv[1],L"--resize-edges")==0)) { evidence=argv[2];std::filesystem::create_directories(evidence); }
    PaletteContrast();
    if(argc>=2 && ::lstrcmpW(argv[1],L"--trim-endpoint")==0) {
        if(argc>=3) { evidence=argv[2]; std::filesystem::create_directories(evidence); }
        try { TrimEndpointPresentation(argc>=4 ? std::filesystem::path(argv[3]) : std::filesystem::path{}); }
        catch(const winrt::hresult_error& error) {
            std::printf("Trim endpoint capture failed: %08lx %ls\n", static_cast<unsigned long>(error.code().value), error.message().c_str());
            ++failures;
        }
    }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--seek-stress")==0) {
        if(argc>=3) { evidence=argv[2];std::filesystem::create_directories(evidence); }
        try { TrimSeekStress(argc>=4 ? static_cast<unsigned>(std::stoul(argv[3])) : 72u); }
        catch(const winrt::hresult_error& error) {
            std::printf("Trim seek capture failed: %08lx %ls\n",static_cast<unsigned long>(error.code().value),error.message().c_str());
            ++failures;
        }
    }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--material-rasters")==0) { MaterialRasterEvidence(); }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--window-lifetime")==0) { RecordingOwnerLifetime(); }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--text-checks")==0) { StandardTextFlows(); EditorFollowupInteractions(); }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--trim-checks")==0) { Trim(); }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--trim-observe")==0) { Trim(false,nullptr,0,false,true); }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--resize-controls")==0) {
        if(argc>=3) { evidence=argv[2];std::filesystem::create_directories(evidence); }
        Trim(true,argc>=4 ? argv[3] : nullptr,0,false,false,false,true);
    }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--resize-external")==0) {
        Trim(true,argc>=3 ? argv[2] : nullptr,0,true,false,true);
        platform::GraphicsEnvironment::Get().Shutdown();::CoUninitialize();
        std::puts("Diagnostic only: visual gate not executed (exit 77), inspect external recording");
        return failures ? 1 : 77;
    }
    else if(argc>=2 && (::lstrcmpW(argv[1],L"--resize-edges")==0 || ::lstrcmpW(argv[1],L"--resize-layout")==0 || ::lstrcmpW(argv[1],L"--resize-unopened")==0 || ::lstrcmpW(argv[1],L"--resize-paused")==0)) {
        if(argc>=3) { evidence=argv[2]; std::filesystem::create_directories(evidence); }
        if(argc>=4) { std::printf("Diagnostic edge filter: %ls (not the full resize gate)\n",argv[3]); }
        Trim(true,argc>=4 ? argv[3] : nullptr,::lstrcmpW(argv[1],L"--resize-unopened")==0 ? 2 : ::lstrcmpW(argv[1],L"--resize-layout")==0 ? 1 : 0,
            ::lstrcmpW(argv[1],L"--resize-paused")==0);
    }
    else if(argc>=2 && ::lstrcmpW(argv[1],L"--editor-checks")==0) { StandardTextFlows(); AnnotationEditing(); InteractionCorrections(); NativeMaterialChecks(); SurfaceStability(); EditorFollowupInteractions(); }
    else { Workspace(); RecordingAndEditor();RecordingOwnerLifetime();Trim(); }
    platform::GraphicsEnvironment::Get().Shutdown();::CoUninitialize();
    std::printf("UI modernization: %d failure(s)\n",failures);return failures?1:0;
}
