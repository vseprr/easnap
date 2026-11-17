#include "ui/icons.h"

#include <objidl.h>

#include <algorithm>
#include <array>
#include <gdiplus.h>

#include "ui/ui_theme.h"

namespace easnap::ui {
namespace {

/// What every icon draws with: GDI+ graphics already scaled to the 24-unit
/// grid, the round-capped stroke pen in the icon colour, and that colour.
struct IconCanvas {
    Gdiplus::Graphics& graphics;
    Gdiplus::Pen& pen;
    COLORREF color;
    void Line(int x, int y, int xx, int yy) { graphics.DrawLine(&pen, x, y, xx, yy); }
    void Box(int x, int y, int xx, int yy) { graphics.DrawRectangle(&pen, x, y, xx - x, yy - y); }
    void Circle(int x, int y, int xx, int yy) { graphics.DrawEllipse(&pen, x, y, xx - x, yy - y); }
};

void DrawIconNone(IconCanvas&) {}
void DrawIconRecord(IconCanvas& canvas) {
    canvas.Circle(4, 4, 20, 20);
    canvas.Circle(8, 8, 16, 16);
}
void DrawIconPause(IconCanvas& canvas) {
    canvas.Box(5, 3, 9, 21);
    canvas.Box(15, 3, 19, 21);
}
void DrawIconPlay(IconCanvas& canvas) {
    canvas.Line(6, 3, 21, 12);
    canvas.Line(21, 12, 6, 21);
    canvas.Line(6, 21, 6, 3);
}
void DrawIconStop(IconCanvas& canvas) {
    canvas.Box(5, 5, 19, 19);
}
void DrawIconSystemAudio(IconCanvas& canvas) {
    canvas.Box(2, 8, 7, 16);
    canvas.Line(7, 8, 13, 3);
    canvas.Line(13, 3, 13, 21);
    canvas.Line(13, 21, 7, 16);
    canvas.Line(17, 7, 20, 10);
    canvas.Line(20, 10, 20, 14);
    canvas.Line(20, 14, 17, 17);
}
void DrawIconMicrophone(IconCanvas& canvas) {
    canvas.Circle(8, 2, 16, 16);
    canvas.Line(5, 11, 5, 15);
    canvas.Line(5, 15, 12, 19);
    canvas.Line(12, 19, 19, 15);
    canvas.Line(19, 15, 19, 11);
    canvas.Line(12, 19, 12, 23);
    canvas.Line(8, 23, 16, 23);
}
void DrawIconCursor(IconCanvas& canvas) {
    canvas.Line(4, 2, 4, 20);
    canvas.Line(4, 20, 9, 15);
    canvas.Line(9, 15, 13, 23);
    canvas.Line(13, 23, 16, 21);
    canvas.Line(16, 21, 12, 14);
    canvas.Line(12, 14, 21, 14);
    canvas.Line(21, 14, 4, 2);
}
void DrawIconCopy(IconCanvas& canvas) {
    canvas.Box(8, 8, 21, 22);
    canvas.Line(15, 5, 15, 2);
    canvas.Line(15, 2, 2, 2);
    canvas.Line(2, 2, 2, 16);
    canvas.Line(2, 16, 5, 16);
}
void DrawIconSave(IconCanvas& canvas) {
    canvas.Line(12, 2, 12, 16);
    canvas.Line(6, 10, 12, 16);
    canvas.Line(12, 16, 18, 10);
    canvas.Line(3, 18, 3, 22);
    canvas.Line(3, 22, 21, 22);
    canvas.Line(21, 22, 21, 18);
}
void DrawIconDiscard(IconCanvas& canvas) {
    canvas.Line(5, 5, 19, 19);
    canvas.Line(5, 19, 19, 5);
}
void DrawIconClear(IconCanvas& canvas) {
    canvas.Line(4, 5, 20, 5);
    canvas.Line(9, 2, 15, 2);
    canvas.Line(6, 8, 7, 21);
    canvas.Line(7, 21, 17, 21);
    canvas.Line(17, 21, 18, 8);
    canvas.Line(10, 9, 10, 18);
    canvas.Line(14, 9, 14, 18);
}
void DrawIconAppearance(IconCanvas& canvas) {
    canvas.Circle(3, 3, 21, 21);
    canvas.Line(12, 3, 12, 21);
    canvas.Line(15, 6, 15, 18);
    canvas.Line(18, 8, 18, 16);
}
void DrawIconPen(IconCanvas& canvas) {
    canvas.Line(3, 21, 6, 13);
    canvas.Line(6, 13, 17, 2);
    canvas.Line(17, 2, 22, 7);
    canvas.Line(22, 7, 11, 18);
    canvas.Line(11, 18, 3, 21);
    canvas.Line(14, 5, 19, 10);
}
void DrawIconArrow(IconCanvas& canvas) {
    canvas.Line(3, 21, 21, 3);
    canvas.Line(11, 3, 21, 3);
    canvas.Line(21, 3, 21, 13);
}
void DrawIconRectangle(IconCanvas& canvas) {
    canvas.Box(2, 5, 22, 19);
}
void DrawIconEllipse(IconCanvas& canvas) {
    canvas.Circle(2, 4, 22, 20);
}
void DrawIconMosaic(IconCanvas& canvas) {
    canvas.Box(2, 2, 22, 22);
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(canvas.color), GetGValue(canvas.color), GetBValue(canvas.color)));
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
            if ((x + y) % 2 == 0) { canvas.graphics.FillRectangle(&fill, 5 + x * 5, 5 + y * 5, 4, 4); }
}
void DrawIconUndo(IconCanvas& canvas) {
    canvas.Line(8, 3, 3, 8);
    canvas.Line(3, 8, 8, 13);
    canvas.graphics.DrawBezier(&canvas.pen, 3, 8, 24, 3, 25, 21, 10, 21);
}
void DrawIconRedo(IconCanvas& canvas) {
    canvas.Line(16, 3, 21, 8);
    canvas.Line(21, 8, 16, 13);
    canvas.graphics.DrawBezier(&canvas.pen, 21, 8, 0, 3, -1, 21, 14, 21);
}
void DrawIconCheck(IconCanvas& canvas) {
    canvas.Line(3, 12, 9, 18);
    canvas.Line(9, 18, 21, 5);
}
void DrawIconLine(IconCanvas& canvas) {
    canvas.Line(4, 20, 20, 4);
}
void DrawIconDroplet(IconCanvas& canvas) {
    canvas.graphics.DrawBezier(&canvas.pen, 12, 2, 11, 7, 3, 11, 5, 17);
    canvas.graphics.DrawBezier(&canvas.pen, 5, 17, 8, 25, 20, 22, 20, 15);
    canvas.graphics.DrawBezier(&canvas.pen, 20, 15, 20, 11, 14, 7, 12, 2);
}
void DrawIconTag(IconCanvas& canvas) {
    canvas.Line(3, 3, 13, 3);
    canvas.Line(13, 3, 22, 12);
    canvas.Line(22, 12, 12, 22);
    canvas.Line(12, 22, 3, 13);
    canvas.Line(3, 13, 3, 3);
    canvas.Circle(6, 6, 9, 9);
}
void DrawIconSpotlight(IconCanvas& canvas) {
    canvas.Box(2, 2, 22, 22);
    canvas.Circle(7, 7, 17, 17);
}
void DrawIconWatermark(IconCanvas& canvas) {
    canvas.Line(2, 10, 10, 2);
    canvas.Line(2, 19, 19, 2);
    canvas.Line(5, 24, 24, 5);
    canvas.Line(14, 24, 24, 14);
}
/// The T shared by the three text-mode icons: solid, knocked out of a
/// rounded square, or outlined with the thinner pen.
void DrawTextGlyph(IconCanvas& canvas, Icon variant) {
    Gdiplus::GraphicsPath glyph;
    glyph.AddLine(5, 5, 19, 5);
    glyph.AddLine(19, 5, 19, 9);
    glyph.AddLine(19, 9, 17, 9);
    glyph.AddLine(17, 9, 17, 7);
    glyph.AddLine(17, 7, 14, 7);
    glyph.AddLine(14, 7, 14, 18);
    glyph.AddLine(14, 18, 17, 18);
    glyph.AddLine(17, 18, 17, 20);
    glyph.AddLine(17, 20, 7, 20);
    glyph.AddLine(7, 20, 7, 18);
    glyph.AddLine(7, 18, 10, 18);
    glyph.AddLine(10, 18, 10, 7);
    glyph.AddLine(10, 7, 7, 7);
    glyph.AddLine(7, 7, 7, 9);
    glyph.AddLine(7, 9, 5, 9);
    glyph.CloseFigure();
    Gdiplus::Color ink(255, GetRValue(canvas.color), GetGValue(canvas.color), GetBValue(canvas.color));
    if (variant == Icon::TextFill) {
        Gdiplus::GraphicsPath square;
        square.AddArc(3, 3, 4, 4, 180, 90);
        square.AddArc(17, 3, 4, 4, 270, 90);
        square.AddArc(17, 17, 4, 4, 0, 90);
        square.AddArc(3, 17, 4, 4, 90, 90);
        square.CloseFigure();
        Gdiplus::SolidBrush background(ink);
        canvas.graphics.FillPath(&background, &square);
        const auto c = theme::Current().panel;
        ink = Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
    }
    if (variant == Icon::TextOutline) {
        canvas.pen.SetWidth(1.2f);
        canvas.graphics.DrawPath(&canvas.pen, &glyph);
    } else {
        Gdiplus::SolidBrush fill(ink);
        canvas.graphics.FillPath(&fill, &glyph);
    }
}
void DrawIconText(IconCanvas& canvas) { DrawTextGlyph(canvas, Icon::Text); }
void DrawIconTextFill(IconCanvas& canvas) { DrawTextGlyph(canvas, Icon::TextFill); }
void DrawIconTextOutline(IconCanvas& canvas) { DrawTextGlyph(canvas, Icon::TextOutline); }
void DrawIconHand(IconCanvas& canvas) {
    Gdiplus::GraphicsPath path;
    path.AddLine(9, 12, 9, 4);
    path.AddBezier(9, 4, 9, 1, 13, 1, 13, 4);
    path.AddLine(13, 4, 13, 10);
    path.AddBezier(13, 10, 19, 9, 22, 12, 21, 16);
    path.AddLine(21, 16, 19, 22);
    path.AddLine(19, 22, 10, 22);
    path.AddLine(10, 22, 3, 15);
    path.AddBezier(3, 15, 1, 11, 5, 10, 9, 14);
    canvas.graphics.DrawPath(&canvas.pen, &path);
}
void DrawIconRegion(IconCanvas& canvas) {
    canvas.Box(5, 5, 19, 19);
    canvas.Box(2, 2, 7, 7);
    canvas.Box(17, 2, 22, 7);
    canvas.Box(2, 17, 7, 22);
    canvas.Box(17, 17, 22, 22);
}
void DrawIconRoundedRectangle(IconCanvas& canvas) {
    Gdiplus::GraphicsPath path;
    path.AddArc(3, 3, 7, 7, 180, 90);
    path.AddArc(14, 3, 7, 7, 270, 90);
    path.AddArc(14, 14, 7, 7, 0, 90);
    path.AddArc(3, 14, 7, 7, 90, 90);
    path.CloseFigure();
    canvas.graphics.DrawPath(&canvas.pen, &path);
}
void DrawIconChevronDown(IconCanvas& canvas) {
    canvas.Line(6, 9, 12, 15);
    canvas.Line(12, 15, 18, 9);
}
void DrawIconMinimize(IconCanvas& canvas) {
    canvas.Line(5, 16, 19, 16);
}
void DrawIconMaximize(IconCanvas& canvas) {
    canvas.Box(5, 5, 19, 19);
}
void DrawIconScissors(IconCanvas& canvas) {
    canvas.Circle(3, 15, 9, 21);
    canvas.Circle(15, 15, 21, 21);
    canvas.Line(8, 16, 19, 3);
    canvas.Line(16, 16, 5, 3);
}
void DrawIconHelp(IconCanvas& canvas) {
    canvas.Circle(3, 3, 21, 21);
    canvas.graphics.DrawBezier(&canvas.pen, 9, 10, 9, 5, 15, 5, 15, 10);
    canvas.graphics.DrawBezier(&canvas.pen, 15, 10, 15, 12, 12, 12, 12, 15);
    Gdiplus::SolidBrush dot(Gdiplus::Color(255, GetRValue(canvas.color), GetGValue(canvas.color), GetBValue(canvas.color)));
    canvas.graphics.FillEllipse(&dot, 11, 17, 2, 2);
}
void DrawIconPin(IconCanvas& canvas) {
    canvas.Line(8, 3, 16, 3);
    canvas.Line(9, 3, 9, 10);
    canvas.Line(15, 3, 15, 10);
    canvas.Line(9, 10, 6, 14);
    canvas.Line(15, 10, 18, 14);
    canvas.Line(6, 14, 18, 14);
    canvas.Line(12, 14, 12, 21);
}

/// One row of the icon table: the icon and the function that draws it.
struct IconPainter {
    Icon icon;
    void (*draw)(IconCanvas&);
};
constexpr std::array kIconPainters{
    IconPainter{Icon::None, DrawIconNone},
    IconPainter{Icon::Record, DrawIconRecord},
    IconPainter{Icon::Pause, DrawIconPause},
    IconPainter{Icon::Play, DrawIconPlay},
    IconPainter{Icon::Stop, DrawIconStop},
    IconPainter{Icon::SystemAudio, DrawIconSystemAudio},
    IconPainter{Icon::Microphone, DrawIconMicrophone},
    IconPainter{Icon::Cursor, DrawIconCursor},
    IconPainter{Icon::Copy, DrawIconCopy},
    IconPainter{Icon::Save, DrawIconSave},
    IconPainter{Icon::Discard, DrawIconDiscard},
    IconPainter{Icon::Appearance, DrawIconAppearance},
    IconPainter{Icon::Pen, DrawIconPen},
    IconPainter{Icon::Arrow, DrawIconArrow},
    IconPainter{Icon::Rectangle, DrawIconRectangle},
    IconPainter{Icon::Ellipse, DrawIconEllipse},
    IconPainter{Icon::Text, DrawIconText},
    IconPainter{Icon::Mosaic, DrawIconMosaic},
    IconPainter{Icon::Undo, DrawIconUndo},
    IconPainter{Icon::Redo, DrawIconRedo},
    IconPainter{Icon::Clear, DrawIconClear},
    IconPainter{Icon::Check, DrawIconCheck},
    IconPainter{Icon::Line, DrawIconLine},
    IconPainter{Icon::Droplet, DrawIconDroplet},
    IconPainter{Icon::Tag, DrawIconTag},
    IconPainter{Icon::Spotlight, DrawIconSpotlight},
    IconPainter{Icon::Watermark, DrawIconWatermark},
    IconPainter{Icon::TextFill, DrawIconTextFill},
    IconPainter{Icon::TextOutline, DrawIconTextOutline},
    IconPainter{Icon::Hand, DrawIconHand},
    IconPainter{Icon::Region, DrawIconRegion},
    IconPainter{Icon::RoundedRectangle, DrawIconRoundedRectangle},
    IconPainter{Icon::ChevronDown, DrawIconChevronDown},
    IconPainter{Icon::Minimize, DrawIconMinimize},
    IconPainter{Icon::Maximize, DrawIconMaximize},
    IconPainter{Icon::Scissors, DrawIconScissors},
    IconPainter{Icon::Help, DrawIconHelp},
    IconPainter{Icon::Pin, DrawIconPin},
};
static_assert(kIconPainters.size() == kIconCount, "one painter per Icon value");
constexpr bool IconPaintersInEnumOrder() {
    for (std::size_t i = 0; i < kIconPainters.size(); ++i) {
        if (static_cast<std::size_t>(kIconPainters[i].icon) != i) { return false; }
    }
    return true;
}
static_assert(IconPaintersInEnumOrder(), "kIconPainters is indexed by Icon");

} // namespace

void DrawIcon(HDC dc, const RECT& r, Icon icon, COLORREF color) {
    static const ULONG_PTR token = [] { ULONG_PTR result{}; Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&result,&input,nullptr); return result; }();
    if (!token) { return; }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<float>(r.left), static_cast<float>(r.top));
    graphics.ScaleTransform((r.right - r.left) / 24.0f, (r.bottom - r.top) / 24.0f);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)), 1.8f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    const auto index = static_cast<std::size_t>(icon);
    if (index >= kIconPainters.size()) { return; }
    IconCanvas canvas{graphics, pen, color};
    kIconPainters[index].draw(canvas);
}

} // namespace easnap::ui
