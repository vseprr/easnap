#include "ui/trim_layout.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace easnap::ui {
namespace {

int ScaleDip(int value, float scale) noexcept {
    return (std::max)(1, static_cast<int>(std::lround(
                             value * (std::max)(0.25f, scale))));
}

bool Contained(const TrimLayoutRect& rect, int width, int height) noexcept {
    return rect.x >= 0 && rect.y >= 0 && rect.width >= 0 && rect.height >= 0 &&
           rect.right() <= width && rect.bottom() <= height;
}

}  // namespace

TrimWindowLayout CalculateTrimWindowLayout(int client_width, int client_height,
                                           float dpi_scale, int status_height,
                                           bool show_progress,
                                           TrimTextMetrics text) noexcept {
    TrimWindowLayout layout;
    if (client_width <= 0 || client_height <= 0) {
        return layout;
    }

    // Keep the real DPI and both button rows. The window enforces the minimum
    // usable size; containment also handles transient smaller WM_SIZE values.
    const float scale = std::max(0.25f, dpi_scale);
    const auto dip = [scale](int value) { return ScaleDip(value, scale); };
    const int margin = dip(12);
    const int gap = dip(6);
    const int width = std::max(0, client_width - 2 * margin);
    const int height = dip(40);
    const int line_height = text.line_height > 0 ? text.line_height : dip(16);
    const int time_height = std::max(dip(22), text.time_height);
    const int range_height = std::max(dip(22), text.range_height);
    // The playhead reaches 23 DIP above the track. Keep its entire extent
    // below a real font-height band, independently of the shorter handles.
    layout.timeline_label_height = line_height;
    layout.timeline_track_center = line_height + dip(2) + dip(23);
    const int timeline_height = layout.timeline_track_center + dip(23);
    const int video_top = dip(kTrimChromeHeightDip);
    const int cell = std::max(0, (width - 2 * gap) / 3);
    const int bottom = std::max(0, client_height - margin - height);
    layout.save_or_cancel = {margin, bottom, cell, height};
    layout.copy = {margin + cell + gap, bottom, cell, height};
    layout.discard = {margin + 2 * (cell + gap), bottom, cell, height};
    int y = bottom - gap;
    const int progress_height=show_progress ? dip(28) : 0;
    y -= progress_height;
    layout.progress = {margin, y, width, progress_height};
    if(progress_height) { y -= gap; }
    // Status may scroll when it cannot fit. Budget against every actual row,
    // including progress and measured readouts, rather than a fixed estimate.
    const int fixed_above_status = video_top + dip(24) + gap + timeline_height +
        gap + time_height + range_height + gap + height + gap;
    const int message_height=std::clamp(status_height,0,std::max(0,y-fixed_above_status));
    y -= message_height;
    layout.status = {margin, y, width, message_height};
    if(message_height) { y -= gap; }
    y -= height;
    layout.play_pause = {margin, y, cell, height};
    layout.set_in = {margin + cell + gap, y, cell, height};
    layout.set_out = {margin + 2 * (cell + gap), y, cell, height};
    y -= gap + range_height;
    layout.range_label = {margin, y, width, range_height};
    y -= time_height;
    layout.time_label={margin,y,width,time_height};
    layout.help = {margin, y, width, 0};
    y -= gap + timeline_height;
    layout.timeline = {margin, y, width, timeline_height};
    layout.title = {0, video_top, 0, 0};
    layout.video = {0, video_top, client_width, std::max(0, y - gap - video_top)};
    // Last-resort containment for transient zero/small WM_SIZE notifications.
    // Supported work areas keep the full control dimensions above.
    for (auto* r : {&layout.title, &layout.time_label, &layout.video, &layout.timeline,
         &layout.help, &layout.range_label, &layout.status, &layout.progress,
         &layout.play_pause, &layout.set_in, &layout.set_out, &layout.save_or_cancel,
         &layout.copy, &layout.discard}) {
        r->x = std::clamp(r->x, 0, client_width);
        r->y = std::clamp(r->y, 0, client_height);
        r->width = std::clamp(r->width, 0, client_width - r->x);
        r->height = std::clamp(r->height, 0, client_height - r->y);
    }
    return layout;
}

TrimLayoutRect FitTrimVideo(TrimLayoutRect area,int source_width,int source_height) noexcept {
    if(source_width<=0 || source_height<=0 || area.width<=0 || area.height<=0) { return area; }
    const double scale=std::min(static_cast<double>(area.width)/source_width,static_cast<double>(area.height)/source_height);
    const int width=std::min(area.width,std::max(1,static_cast<int>(std::lround(source_width*scale))));
    const int height=std::min(area.height,std::max(1,static_cast<int>(std::lround(source_height*scale))));
    return {area.x+(area.width-width)/2,area.y+(area.height-height)/2,width,height};
}

TrimLayoutRect InitialTrimWindowSize(int work_width,int work_height,float dpi_scale,int source_width,int source_height) noexcept {
    const auto dip=[dpi_scale](int value) { return ScaleDip(value,std::max(.25f,dpi_scale)); };
    const int max_width=std::max(1,work_width*9/10),max_height=std::max(1,work_height*9/10);
    const auto layout=CalculateTrimWindowLayout(max_width,max_height,dpi_scale);
    const auto fitted=FitTrimVideo(layout.video,source_width,source_height);
    const int width=std::min(max_width,std::max(dip(560),fitted.width));
    // Recalculate with the usable UI width before deciding the final height.
    const auto chosen=CalculateTrimWindowLayout(width,max_height,dpi_scale);
    const auto picture=FitTrimVideo(chosen.video,source_width,source_height);
    const int height=std::min(max_height,std::max(dip(kTrimMinClientHeightDip),picture.height+max_height-chosen.video.height));
    return {0,0,width,height};
}

bool IsTrimWindowLayoutContained(const TrimWindowLayout& layout,
                                 int client_width,
                                 int client_height) noexcept {
    const std::array<const TrimLayoutRect*, 14> rectangles{
        &layout.title,
        &layout.time_label,
        &layout.video,
        &layout.range_label,
        &layout.timeline,
        &layout.help,
        &layout.status,
        &layout.progress,
        &layout.play_pause,
        &layout.set_in,
        &layout.set_out,
        &layout.save_or_cancel,
        &layout.copy,
        &layout.discard,
    };
    return std::all_of(rectangles.begin(), rectangles.end(),
                       [client_width, client_height](const auto* rect) {
                           return Contained(*rect, client_width, client_height);
                       });
}

}  // namespace easnap::ui
