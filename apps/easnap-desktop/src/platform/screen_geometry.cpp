// Easnap - screen geometry implementation.
#include "platform/screen_geometry.h"

#include <shellscalingapi.h>

#include <algorithm>

namespace easnap::platform {

namespace {

constexpr UINT kDefaultDpi = 96;

/// Dynamically resolved APIs. Easnap probes for capabilities instead of
/// branching on Windows version numbers.
struct DynamicApis {
    using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(PROCESS_DPI_AWARENESS);

    SetProcessDpiAwarenessContextFn set_process_dpi_awareness_context{nullptr};
    SetProcessDpiAwarenessFn set_process_dpi_awareness{nullptr};
    GetDpiForMonitorFn get_dpi_for_monitor{nullptr};

    DynamicApis() {
        if (HMODULE user32 = ::GetModuleHandleW(L"user32.dll")) {
            set_process_dpi_awareness_context = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                reinterpret_cast<void*>(::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        }
        if (HMODULE shcore = ::LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            get_dpi_for_monitor = reinterpret_cast<GetDpiForMonitorFn>(
                reinterpret_cast<void*>(::GetProcAddress(shcore, "GetDpiForMonitor")));
            set_process_dpi_awareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
                reinterpret_cast<void*>(::GetProcAddress(shcore, "SetProcessDpiAwareness")));
        }
    }
};

const DynamicApis& Apis() {
    static const DynamicApis apis;
    return apis;
}

UINT DpiForMonitor(HMONITOR monitor) {
    UINT dpi_x = kDefaultDpi;
    UINT dpi_y = kDefaultDpi;
    if (Apis().get_dpi_for_monitor != nullptr &&
        SUCCEEDED(Apis().get_dpi_for_monitor(monitor, MDT_EFFECTIVE_DPI, &dpi_x, &dpi_y))) {
        return dpi_x;
    }
    return kDefaultDpi;
}

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info) == FALSE) {
        return TRUE;
    }
    MonitorInfo entry{};
    entry.handle = monitor;
    entry.bounds = info.rcMonitor;
    entry.work_area = info.rcWork;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    entry.dpi = DpiForMonitor(monitor);
    monitors->push_back(entry);
    return TRUE;
}

}  // namespace

// ---------------------------------------------------------------------------
// DPI and monitors
// ---------------------------------------------------------------------------

void EnableDpiAwareness() {
    // The product executables declare per-monitor v2 in their manifests, which
    // makes the calls below fail harmlessly; they serve manifest-less tests.
    if (Apis().set_process_dpi_awareness_context != nullptr &&
        Apis().set_process_dpi_awareness_context(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE) {
        return;
    }
    if (Apis().set_process_dpi_awareness != nullptr &&
        SUCCEEDED(Apis().set_process_dpi_awareness(PROCESS_PER_MONITOR_DPI_AWARE))) {
        return;
    }
    ::SetProcessDPIAware();
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    ::EnumDisplayMonitors(nullptr, nullptr, &MonitorEnumProc, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) {
        // Extremely defensive fallback: synthesize a primary monitor entry.
        MonitorInfo primary{};
        primary.bounds = RECT{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
        primary.work_area = primary.bounds;
        primary.primary = true;
        monitors.push_back(primary);
    }
    return monitors;
}

MonitorInfo MonitorForHandle(HMONITOR handle) {
    const std::vector<MonitorInfo> monitors = EnumerateMonitors();
    for (const MonitorInfo& monitor : monitors) {
        if (monitor.handle == handle) {
            return monitor;
        }
    }
    return monitors.front();
}

MonitorInfo MonitorForPoint(POINT pt) {
    return MonitorForHandle(::MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST));
}

MonitorInfo MonitorForRect(const RECT& rect) {
    return MonitorForHandle(::MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST));
}

RECT VirtualScreenBounds() {
    RECT bounds{};
    bounds.left = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    bounds.top = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    bounds.right = bounds.left + ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    bounds.bottom = bounds.top + ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    return bounds;
}

RECT NormalizedRect(POINT a, POINT b) {
    RECT r{};
    r.left = std::min(a.x, b.x);
    r.top = std::min(a.y, b.y);
    r.right = std::max(a.x, b.x);
    r.bottom = std::max(a.y, b.y);
    return r;
}

RECT IntersectRects(const RECT& a, const RECT& b) {
    RECT out{};
    if (::IntersectRect(&out, &a, &b) == FALSE) {
        return RECT{0, 0, 0, 0};
    }
    return out;
}

bool IsEmptyRect(const RECT& r) noexcept { return r.right <= r.left || r.bottom <= r.top; }

RECT ClampRectToBounds(RECT rect, const RECT& bounds) {
    const LONG width = std::min(RectWidth(rect), RectWidth(bounds));
    const LONG height = std::min(RectHeight(rect), RectHeight(bounds));
    rect.left = std::clamp(rect.left, bounds.left, bounds.right - width);
    rect.top = std::clamp(rect.top, bounds.top, bounds.bottom - height);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

}  // namespace easnap::platform
