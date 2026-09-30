#pragma once

#include <array>
#include <functional>
#include <memory>
#include "platform/platform_utils.h"

namespace easnap::ui {

struct AccessibleTimelineValue {
    double value{}, minimum{}, maximum{}; // Seconds; not a claim of codec precision.
    RECT bounds{};
};
struct AccessibleTimelineSnapshot {
    std::array<AccessibleTimelineValue, 3> values{}; // In, playhead, Out.
    bool enabled{false};
    int focus{1};
};

// STA providers, owned by the timeline HWND, with detachable callback lifetime.
// Automation clients may retain providers after the production window closes.
class TimelineAccessibility final {
public:
    TimelineAccessibility(HWND window, std::function<AccessibleTimelineSnapshot()> read,
        std::function<bool(int, double)> set, std::function<void(int)> focus);
    ~TimelineAccessibility();
    TimelineAccessibility(const TimelineAccessibility&) = delete;
    TimelineAccessibility& operator=(const TimelineAccessibility&) = delete;
    LRESULT HandleGetObject(WPARAM wparam, LPARAM lparam);
    void NotifyChanged();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace easnap::ui
