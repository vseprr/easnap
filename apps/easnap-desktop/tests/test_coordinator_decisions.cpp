// Easnap unit tests - decisions the capture coordinator takes from facts.
//
// The coordinator applies these choices to windows; the choices themselves are
// pure functions of the session's facts and are checked here against tables
// written from the behaviour the coordinator had when they were extracted. No
// window, monitor or device is used.

#include "app/scrollshot_status.h"

#include <cstdio>
#include <optional>

#include "string_ids.h"

using easnap::app::ChooseScrollshotStatus;
using easnap::app::ScrollshotNotice;
using easnap::app::ScrollshotStatus;
using easnap::scrollshot::ScrollshotCondition;

namespace {

int g_pass = 0;
int g_fail = 0;
const char* g_case = "";

void Check(bool condition, const char* expression, const char* file, int line) {
    if (condition) {
        ++g_pass;
    } else {
        ++g_fail;
        std::fprintf(stderr, "FAIL  %s:%d  [%s]  %s\n", file, line, g_case, expression);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Scrollshot island message line

struct StatusCase {
    const char* name;
    std::optional<ScrollshotCondition> capture;
    ScrollshotNotice notice;
    UINT message;
    bool warning;
};

constexpr std::optional<ScrollshotCondition> kStopped{};
constexpr std::optional<ScrollshotCondition> kFollowing{ScrollshotCondition::Following};
constexpr std::optional<ScrollshotCondition> kLost{ScrollshotCondition::Lost};
constexpr std::optional<ScrollshotCondition> kAtLimit{ScrollshotCondition::AtLimit};
constexpr std::optional<ScrollshotCondition> kCaptureFailed{ScrollshotCondition::CaptureFailed};

// Every capture condition (and no capture) against every notice. Notes:
// the wheel notice shows only while a capture follows; DisplayChanged, Copying
// and Saving keep the warning of the condition beneath them.
const StatusCase kStatusCases[] = {
    {"stopped/none", kStopped, ScrollshotNotice::None, IDS_SCROLLSHOT_STATUS_READY, false},
    {"stopped/wheel", kStopped, ScrollshotNotice::WheelBlocked, IDS_SCROLLSHOT_STATUS_READY, false},
    {"stopped/display", kStopped, ScrollshotNotice::DisplayChanged, IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED, false},
    {"stopped/stopping", kStopped, ScrollshotNotice::Stopping, IDS_SCROLLSHOT_STATUS_STOPPING, true},
    {"stopped/copying", kStopped, ScrollshotNotice::Copying, IDS_SCROLLSHOT_STATUS_COPYING, false},
    {"stopped/saving", kStopped, ScrollshotNotice::Saving, IDS_SCROLLSHOT_STATUS_SAVING, false},
    {"stopped/copy-failed", kStopped, ScrollshotNotice::CopyFailed, IDS_SCROLLSHOT_STATUS_COPY_FAILED, true},
    {"stopped/save-failed", kStopped, ScrollshotNotice::SaveFailed, IDS_SCROLLSHOT_STATUS_SAVE_FAILED, true},

    {"following/none", kFollowing, ScrollshotNotice::None, IDS_SCROLLSHOT_STATUS_SCROLL, false},
    {"following/wheel", kFollowing, ScrollshotNotice::WheelBlocked, IDS_SCROLLSHOT_STATUS_WHEEL, false},
    {"following/display", kFollowing, ScrollshotNotice::DisplayChanged, IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED,
     false},
    {"following/stopping", kFollowing, ScrollshotNotice::Stopping, IDS_SCROLLSHOT_STATUS_STOPPING, true},
    {"following/copying", kFollowing, ScrollshotNotice::Copying, IDS_SCROLLSHOT_STATUS_COPYING, false},
    {"following/saving", kFollowing, ScrollshotNotice::Saving, IDS_SCROLLSHOT_STATUS_SAVING, false},
    {"following/copy-failed", kFollowing, ScrollshotNotice::CopyFailed, IDS_SCROLLSHOT_STATUS_COPY_FAILED, true},
    {"following/save-failed", kFollowing, ScrollshotNotice::SaveFailed, IDS_SCROLLSHOT_STATUS_SAVE_FAILED, true},

    {"lost/none", kLost, ScrollshotNotice::None, IDS_SCROLLSHOT_STATUS_LOST, true},
    {"lost/wheel", kLost, ScrollshotNotice::WheelBlocked, IDS_SCROLLSHOT_STATUS_LOST, true},
    {"lost/display", kLost, ScrollshotNotice::DisplayChanged, IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED, true},
    {"lost/stopping", kLost, ScrollshotNotice::Stopping, IDS_SCROLLSHOT_STATUS_STOPPING, true},
    {"lost/copying", kLost, ScrollshotNotice::Copying, IDS_SCROLLSHOT_STATUS_COPYING, true},
    {"lost/saving", kLost, ScrollshotNotice::Saving, IDS_SCROLLSHOT_STATUS_SAVING, true},
    {"lost/copy-failed", kLost, ScrollshotNotice::CopyFailed, IDS_SCROLLSHOT_STATUS_COPY_FAILED, true},
    {"lost/save-failed", kLost, ScrollshotNotice::SaveFailed, IDS_SCROLLSHOT_STATUS_SAVE_FAILED, true},

    {"limit/none", kAtLimit, ScrollshotNotice::None, IDS_SCROLLSHOT_STATUS_LIMIT, false},
    {"limit/wheel", kAtLimit, ScrollshotNotice::WheelBlocked, IDS_SCROLLSHOT_STATUS_LIMIT, false},
    {"limit/display", kAtLimit, ScrollshotNotice::DisplayChanged, IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED, false},
    {"limit/stopping", kAtLimit, ScrollshotNotice::Stopping, IDS_SCROLLSHOT_STATUS_STOPPING, true},
    {"limit/copying", kAtLimit, ScrollshotNotice::Copying, IDS_SCROLLSHOT_STATUS_COPYING, false},
    {"limit/saving", kAtLimit, ScrollshotNotice::Saving, IDS_SCROLLSHOT_STATUS_SAVING, false},
    {"limit/copy-failed", kAtLimit, ScrollshotNotice::CopyFailed, IDS_SCROLLSHOT_STATUS_COPY_FAILED, true},
    {"limit/save-failed", kAtLimit, ScrollshotNotice::SaveFailed, IDS_SCROLLSHOT_STATUS_SAVE_FAILED, true},

    {"capture-failed/none", kCaptureFailed, ScrollshotNotice::None, IDS_SCROLLSHOT_STATUS_CAPTURE_FAILED, true},
    {"capture-failed/wheel", kCaptureFailed, ScrollshotNotice::WheelBlocked, IDS_SCROLLSHOT_STATUS_CAPTURE_FAILED,
     true},
    {"capture-failed/display", kCaptureFailed, ScrollshotNotice::DisplayChanged,
     IDS_SCROLLSHOT_STATUS_DISPLAY_CHANGED, true},
    {"capture-failed/stopping", kCaptureFailed, ScrollshotNotice::Stopping, IDS_SCROLLSHOT_STATUS_STOPPING, true},
    {"capture-failed/copying", kCaptureFailed, ScrollshotNotice::Copying, IDS_SCROLLSHOT_STATUS_COPYING, true},
    {"capture-failed/saving", kCaptureFailed, ScrollshotNotice::Saving, IDS_SCROLLSHOT_STATUS_SAVING, true},
    {"capture-failed/copy-failed", kCaptureFailed, ScrollshotNotice::CopyFailed, IDS_SCROLLSHOT_STATUS_COPY_FAILED,
     true},
    {"capture-failed/save-failed", kCaptureFailed, ScrollshotNotice::SaveFailed, IDS_SCROLLSHOT_STATUS_SAVE_FAILED,
     true},
};

void TestScrollshotStatus() {
    int cases = 0;
    for (const StatusCase& item : kStatusCases) {
        g_case = item.name;
        const ScrollshotStatus status = ChooseScrollshotStatus(item.capture, item.notice);
        CHECK(status.message == item.message);
        CHECK(status.warning == item.warning);
        ++cases;
    }
    g_case = "status table";
    CHECK(cases == 40);  // 5 capture conditions x 8 notices.
}

}  // namespace

int main() {
    TestScrollshotStatus();
    std::printf("coordinator decisions: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
