#pragma once
#include <cstdio>
#include <optional>
#include <stdexcept>
#include "platform/platform_utils.h"

namespace easnap::test {
// Opt-in physical display selection for interactive tests. Fail explicitly if
// the requested display is disconnected; never silently test another monitor.
inline const platform::MonitorInfo* SelectedTestMonitor() {
    static const auto selected=[]() -> std::optional<platform::MonitorInfo> {
        wchar_t name[64]{};
        const DWORD length=::GetEnvironmentVariableW(L"EASNAP_TEST_MONITOR",name,64);
        if(!length) return std::nullopt;
        if(length>=64) throw std::runtime_error("EASNAP_TEST_MONITOR is too long");
        for(const auto& monitor:platform::EnumerateMonitors()) {
            MONITORINFOEXW info{};info.cbSize=sizeof(info);
            if(::GetMonitorInfoW(monitor.handle,&info) && ::lstrcmpiW(name,info.szDevice)==0) {
                std::printf("Test monitor: %ls bounds=%ld,%ld,%ld,%ld dpi=%u\n",name,
                    monitor.bounds.left,monitor.bounds.top,monitor.bounds.right,monitor.bounds.bottom,monitor.dpi);
                return monitor;
            }
        }
        throw std::runtime_error("EASNAP_TEST_MONITOR is not an active independent display");
    }();
    return selected ? &*selected : nullptr;
}
inline RECT OnTestMonitor(RECT rect) {
    if(const auto* monitor=SelectedTestMonitor()) ::OffsetRect(&rect,monitor->bounds.left,monitor->bounds.top);
    return rect;
}
inline POINT OnTestPoint(POINT point) {
    if(const auto* monitor=SelectedTestMonitor()) {
        point.x+=monitor->bounds.left;point.y+=monitor->bounds.top;
    }
    return point;
}
}
