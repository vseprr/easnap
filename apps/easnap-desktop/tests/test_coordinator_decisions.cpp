// Easnap unit tests - decisions the capture coordinator takes from facts.
//
// The coordinator applies these choices to windows; the choices themselves are
// pure functions of the session's facts and are checked here against tables
// written from the behaviour the coordinator had when they were extracted. No
// window, monitor or device is used.

#include "app/display_change.h"
#include "app/scrollshot_status.h"
#include "ui/image_export_outcome.h"

#include <cstdio>
#include <optional>

#include "string_ids.h"

using easnap::app::ChooseScrollshotStatus;
using easnap::app::DecideDisplayChange;
using easnap::app::DisplayChangeAction;
using easnap::app::DisplayChangeFacts;
using easnap::app::SessionState;
using easnap::app::ScrollshotNotice;
using easnap::app::ScrollshotStatus;
using easnap::scrollshot::ScrollshotCondition;
using easnap::ui::ChooseImageExportOutcome;
using Outcome = easnap::ui::ImageExportOutcome;
using Status = easnap::platform::ImageExportStatus;
using Target = easnap::platform::ImageExportTarget;

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

// ---------------------------------------------------------------------------
// Display change: the first rule that applies decides

constexpr SessionState kEveryState[] = {
    SessionState::Idle,
    SessionState::RegionSelecting,
    SessionState::ScreenshotEditing,
    SessionState::ScreenshotExporting,
    SessionState::RecordingConfiguring,
    SessionState::RecordingActive,
    SessionState::RecordingPaused,
    SessionState::RecordingFinalizing,
    SessionState::RecordingTrimming,
    SessionState::Exporting,
    SessionState::ScrollshotConfiguring,
    SessionState::ScrollshotCapturing,
    SessionState::ScrollshotReview,
};

// Every fact that could lead to an action is set: what wins is the rule order.
[[nodiscard]] DisplayChangeFacts Everything(SessionState state) {
    return DisplayChangeFacts{.state = state,
                              .export_running = true,
                              .layout_changed = true,
                              .editing_document = true,
                              .workspace_visible = true,
                              .recording_region_on_desktop = false,
                              .trim_window_visible = true};
}

void TestIdleAndRunningExport() {
    g_case = "idle ignores everything";
    CHECK(DecideDisplayChange(Everything(SessionState::Idle)) == DisplayChangeAction::None);
    CHECK(DecideDisplayChange(DisplayChangeFacts{}) == DisplayChangeAction::None);
    g_case = "a running export defers every session";
    for (const SessionState state : kEveryState) {
        if (state == SessionState::Idle) { continue; }
        CHECK(DecideDisplayChange(Everything(state)) == DisplayChangeAction::RecheckAfterExport);
        const DisplayChangeFacts quiet{.state = state, .export_running = true};
        CHECK(DecideDisplayChange(quiet) == DisplayChangeAction::RecheckAfterExport);
    }
}

void TestScrollshot() {
    for (const SessionState state : {SessionState::ScrollshotCapturing, SessionState::ScrollshotReview}) {
        g_case = "scrollshot keeps its image when the layout changed";
        DisplayChangeFacts facts = Everything(state);
        facts.export_running = false;
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::KeepScrollshotImage);
        g_case = "scrollshot with the same layout: nothing, not the workspace rule";
        facts.layout_changed = false;
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::None);
    }
    g_case = "scrollshot frame still movable: only the workspace rule";
    DisplayChangeFacts configuring{
        .state = SessionState::ScrollshotConfiguring, .layout_changed = true, .workspace_visible = true};
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::CancelSession);
    configuring.layout_changed = false;
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::None);
    configuring.layout_changed = true;
    configuring.workspace_visible = false;
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::None);
}

void TestScreenshotEditing() {
    g_case = "an edited stitched image goes back to review";
    DisplayChangeFacts facts = Everything(SessionState::ScreenshotEditing);
    facts.export_running = false;
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::ReturnDocumentToReview);
    g_case = "document with the same layout: nothing, even with the workspace visible";
    facts.layout_changed = false;
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::None);
    g_case = "editing a desktop region: the workspace rule";
    facts.editing_document = false;
    facts.layout_changed = true;
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::CancelSession);
    facts.layout_changed = false;
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::None);
    facts.layout_changed = true;
    facts.workspace_visible = false;
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::None);
    g_case = "a document only counts while editing";
    for (const SessionState state : {SessionState::RegionSelecting, SessionState::ScreenshotExporting}) {
        const DisplayChangeFacts other{.state = state, .layout_changed = true, .editing_document = true};
        CHECK(DecideDisplayChange(other) == DisplayChangeAction::None);
    }
}

void TestSelection() {
    g_case = "a visible selection is cancelled only when the layout changed";
    DisplayChangeFacts facts{.state = SessionState::RegionSelecting, .layout_changed = true, .workspace_visible = true};
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::CancelSession);
    facts.layout_changed = false;  // A work-area change leaves the snapshot valid.
    CHECK(DecideDisplayChange(facts) == DisplayChangeAction::None);
    g_case = "exporting a screenshot: nothing";
    const DisplayChangeFacts exporting{.state = SessionState::ScreenshotExporting, .layout_changed = true};
    CHECK(DecideDisplayChange(exporting) == DisplayChangeAction::None);
}

void TestRecording() {
    g_case = "a visible workspace wins over the recording rule";
    DisplayChangeFacts configuring = Everything(SessionState::RecordingConfiguring);
    configuring.export_running = false;
    configuring.editing_document = false;
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::CancelSession);
    configuring.layout_changed = false;  // Same layout: the region is not looked at.
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::None);

    g_case = "a region that left the desktop ends the recording";
    configuring.workspace_visible = false;
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::CancelRecordingConfiguration);
    for (const SessionState state : {SessionState::RecordingActive, SessionState::RecordingPaused}) {
        g_case = "a region that left the desktop ends the recording";
        DisplayChangeFacts facts{.state = state, .recording_region_on_desktop = false, .trim_window_visible = true};
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::StopRecording);
        g_case = "a region still on the desktop keeps the recording";
        facts.recording_region_on_desktop = true;
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::RefreshTrimLayout);
        facts.trim_window_visible = false;
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::RefreshRecordingControlsLayout);
    }
    g_case = "configuring on the desktop keeps the recording";
    configuring.recording_region_on_desktop = true;
    configuring.trim_window_visible = false;
    CHECK(DecideDisplayChange(configuring) == DisplayChangeAction::RefreshRecordingControlsLayout);

    g_case = "after capture the region is not looked at";
    for (const SessionState state :
         {SessionState::RecordingFinalizing, SessionState::RecordingTrimming, SessionState::Exporting}) {
        DisplayChangeFacts facts{.state = state, .layout_changed = true, .recording_region_on_desktop = false};
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::RefreshRecordingControlsLayout);
        facts.trim_window_visible = true;
        CHECK(DecideDisplayChange(facts) == DisplayChangeAction::RefreshTrimLayout);
    }
}

// ---------------------------------------------------------------------------
// A finished copy or save, shared by the editor and the Scrollshot island

struct ExportCase {
    Status status;
    Target target;
    bool published;  // Only a prepared copy can reach the clipboard.
    bool quick;
    bool can_offer_dialog;
    Outcome outcome;
};

const ExportCase kExportCases[] = {
    {Status::Succeeded, Target::Clipboard, true, false, false, Outcome::Copied},
    {Status::Succeeded, Target::Clipboard, true, false, true, Outcome::Copied},
    {Status::Succeeded, Target::Clipboard, true, true, false, Outcome::Copied},
    {Status::Succeeded, Target::Clipboard, true, true, true, Outcome::Copied},
    // Prepared, but the clipboard could not be opened.
    {Status::Succeeded, Target::Clipboard, false, false, false, Outcome::CopyFailed},
    {Status::Succeeded, Target::Clipboard, false, false, true, Outcome::CopyFailed},
    {Status::Succeeded, Target::Clipboard, false, true, false, Outcome::CopyFailed},
    {Status::Succeeded, Target::Clipboard, false, true, true, Outcome::CopyFailed},
    {Status::Succeeded, Target::File, false, false, false, Outcome::Saved},
    {Status::Succeeded, Target::File, false, false, true, Outcome::Saved},
    {Status::Succeeded, Target::File, false, true, false, Outcome::Saved},
    {Status::Succeeded, Target::File, false, true, true, Outcome::Saved},
    {Status::Failed, Target::Clipboard, false, false, false, Outcome::CopyFailed},
    {Status::Failed, Target::Clipboard, false, false, true, Outcome::CopyFailed},
    {Status::Failed, Target::Clipboard, false, true, false, Outcome::CopyFailed},
    {Status::Failed, Target::Clipboard, false, true, true, Outcome::CopyFailed},
    {Status::Failed, Target::File, false, false, false, Outcome::SaveFailed},
    {Status::Failed, Target::File, false, false, true, Outcome::SaveFailed},
    {Status::Failed, Target::File, false, true, false, Outcome::SaveFailed},
    // Only a failed quick save, while the window can still ask, offers the dialog.
    {Status::Failed, Target::File, false, true, true, Outcome::RetryWithDialog},
    {Status::Cancelled, Target::Clipboard, false, false, false, Outcome::Cancelled},
    {Status::Cancelled, Target::Clipboard, false, false, true, Outcome::Cancelled},
    {Status::Cancelled, Target::Clipboard, false, true, false, Outcome::Cancelled},
    {Status::Cancelled, Target::Clipboard, false, true, true, Outcome::Cancelled},
    {Status::Cancelled, Target::File, false, false, false, Outcome::Cancelled},
    {Status::Cancelled, Target::File, false, false, true, Outcome::Cancelled},
    {Status::Cancelled, Target::File, false, true, false, Outcome::Cancelled},
    {Status::Cancelled, Target::File, false, true, true, Outcome::Cancelled},
};

void TestImageExportOutcome() {
    g_case = "image export outcome";
    int cases = 0;
    for (const ExportCase& item : kExportCases) {
        CHECK(ChooseImageExportOutcome(item.status, item.target, item.published, item.quick, item.can_offer_dialog) ==
              item.outcome);
        ++cases;
    }
    CHECK(cases == 28);  // 3 statuses x 2 targets x quick x dialog, and a published copy.
}

}  // namespace

int main() {
    TestScrollshotStatus();
    TestIdleAndRunningExport();
    TestScrollshot();
    TestScreenshotEditing();
    TestSelection();
    TestRecording();
    TestImageExportOutcome();
    std::printf("coordinator decisions: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
