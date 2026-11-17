#include "ui/icons.h"

#include <objidl.h>

#include <algorithm>
#include <gdiplus.h>

#include "ui/ui_theme.h"

namespace easnap::ui {

void DrawIcon(HDC dc, const RECT& r, Icon icon, COLORREF color, float scale) {
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
    const auto line = [&](int x, int y, int xx, int yy) { graphics.DrawLine(&pen, x, y, xx, yy); };
    const auto box = [&](int x, int y, int xx, int yy) { graphics.DrawRectangle(&pen, x, y, xx - x, yy - y); };
    const auto circle = [&](int x, int y, int xx, int yy) { graphics.DrawEllipse(&pen, x, y, xx - x, yy - y); };
    (void)scale;
    switch (icon) {
        case Icon::Record:
            circle(4, 4, 20, 20);
            circle(8, 8, 16, 16);
            break;
        case Icon::Pause:
            box(5, 3, 9, 21);
            box(15, 3, 19, 21);
            break;
        case Icon::Play:
            line(6, 3, 21, 12);
            line(21, 12, 6, 21);
            line(6, 21, 6, 3);
            break;
        case Icon::Stop: box(5, 5, 19, 19); break;
        case Icon::SystemAudio:
            box(2, 8, 7, 16);
            line(7, 8, 13, 3);
            line(13, 3, 13, 21);
            line(13, 21, 7, 16);
            line(17, 7, 20, 10);
            line(20, 10, 20, 14);
            line(20, 14, 17, 17);
            break;
        case Icon::Microphone:
            circle(8, 2, 16, 16);
            line(5, 11, 5, 15);
            line(5, 15, 12, 19);
            line(12, 19, 19, 15);
            line(19, 15, 19, 11);
            line(12, 19, 12, 23);
            line(8, 23, 16, 23);
            break;
        case Icon::Cursor:
            line(4, 2, 4, 20);
            line(4, 20, 9, 15);
            line(9, 15, 13, 23);
            line(13, 23, 16, 21);
            line(16, 21, 12, 14);
            line(12, 14, 21, 14);
            line(21, 14, 4, 2);
            break;
        case Icon::Copy:
            box(8, 8, 21, 22);
            line(15, 5, 15, 2);
            line(15, 2, 2, 2);
            line(2, 2, 2, 16);
            line(2, 16, 5, 16);
            break;
        case Icon::Save:
            line(12, 2, 12, 16);
            line(6, 10, 12, 16);
            line(12, 16, 18, 10);
            line(3, 18, 3, 22);
            line(3, 22, 21, 22);
            line(21, 22, 21, 18);
            break;
        case Icon::Discard:
            line(5, 5, 19, 19);
            line(5, 19, 19, 5);
            break;
        case Icon::Clear:
            line(4, 5, 20, 5);
            line(9, 2, 15, 2);
            line(6, 8, 7, 21);
            line(7, 21, 17, 21);
            line(17, 21, 18, 8);
            line(10, 9, 10, 18);
            line(14, 9, 14, 18);
            break;
        case Icon::Appearance:
            circle(3, 3, 21, 21);
            line(12, 3, 12, 21);
            line(15, 6, 15, 18);
            line(18, 8, 18, 16);
            break;
        case Icon::Sun:
            circle(7, 7, 17, 17);
            line(12, 0, 12, 4);
            line(12, 20, 12, 24);
            line(0, 12, 4, 12);
            line(20, 12, 24, 12);
            line(3, 3, 5, 5);
            line(19, 19, 21, 21);
            line(3, 21, 5, 19);
            line(19, 5, 21, 3);
            break;
        case Icon::Moon:
            circle(3, 3, 21, 21);
            line(12, 3, 8, 9);
            line(8, 9, 11, 16);
            line(11, 16, 21, 17);
            break;
        case Icon::Pen:
            line(3, 21, 6, 13);
            line(6, 13, 17, 2);
            line(17, 2, 22, 7);
            line(22, 7, 11, 18);
            line(11, 18, 3, 21);
            line(14, 5, 19, 10);
            break;
        case Icon::Arrow:
            line(3, 21, 21, 3);
            line(11, 3, 21, 3);
            line(21, 3, 21, 13);
            break;
        case Icon::Rectangle: box(2, 5, 22, 19); break;
        case Icon::Ellipse: circle(2, 4, 22, 20); break;
        case Icon::Mosaic: {
            box(2, 2, 22, 22);
            Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
            for (int y = 0; y < 3; ++y)
                for (int x = 0; x < 3; ++x)
                    if ((x + y) % 2 == 0) { graphics.FillRectangle(&fill, 5 + x * 5, 5 + y * 5, 4, 4); }
            break;
        }
        case Icon::Undo:
            line(8, 3, 3, 8);
            line(3, 8, 8, 13);
            graphics.DrawBezier(&pen, 3, 8, 24, 3, 25, 21, 10, 21);
            break;
        case Icon::Redo:
            line(16, 3, 21, 8);
            line(21, 8, 16, 13);
            graphics.DrawBezier(&pen, 21, 8, 0, 3, -1, 21, 14, 21);
            break;
        case Icon::Check:
            line(3, 12, 9, 18);
            line(9, 18, 21, 5);
            break;
        case Icon::Line: line(4, 20, 20, 4); break;
        case Icon::Droplet:
            graphics.DrawBezier(&pen, 12, 2, 11, 7, 3, 11, 5, 17);
            graphics.DrawBezier(&pen, 5, 17, 8, 25, 20, 22, 20, 15);
            graphics.DrawBezier(&pen, 20, 15, 20, 11, 14, 7, 12, 2);
            break;
        case Icon::Tag:
            line(3, 3, 13, 3);
            line(13, 3, 22, 12);
            line(22, 12, 12, 22);
            line(12, 22, 3, 13);
            line(3, 13, 3, 3);
            circle(6, 6, 9, 9);
            break;
        case Icon::Spotlight:
            box(2, 2, 22, 22);
            circle(7, 7, 17, 17);
            break;
        case Icon::Watermark:
            line(2, 10, 10, 2);
            line(2, 19, 19, 2);
            line(5, 24, 24, 5);
            line(14, 24, 24, 14);
            break;
        case Icon::Text:
        case Icon::TextFill:
        case Icon::TextOutline: {
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
            Gdiplus::Color ink(255, GetRValue(color), GetGValue(color), GetBValue(color));
            if (icon == Icon::TextFill) {
                Gdiplus::GraphicsPath square;
                square.AddArc(3, 3, 4, 4, 180, 90);
                square.AddArc(17, 3, 4, 4, 270, 90);
                square.AddArc(17, 17, 4, 4, 0, 90);
                square.AddArc(3, 17, 4, 4, 90, 90);
                square.CloseFigure();
                Gdiplus::SolidBrush background(ink);
                graphics.FillPath(&background, &square);
                const auto c = theme::Current().panel;
                ink = Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c));
            }
            if (icon == Icon::TextOutline) {
                pen.SetWidth(1.2f);
                graphics.DrawPath(&pen, &glyph);
            } else {
                Gdiplus::SolidBrush fill(ink);
                graphics.FillPath(&fill, &glyph);
            }
            break;
        }
        case Icon::Hand: {
            Gdiplus::GraphicsPath path;
            path.AddLine(9, 12, 9, 4);
            path.AddBezier(9, 4, 9, 1, 13, 1, 13, 4);
            path.AddLine(13, 4, 13, 10);
            path.AddBezier(13, 10, 19, 9, 22, 12, 21, 16);
            path.AddLine(21, 16, 19, 22);
            path.AddLine(19, 22, 10, 22);
            path.AddLine(10, 22, 3, 15);
            path.AddBezier(3, 15, 1, 11, 5, 10, 9, 14);
            graphics.DrawPath(&pen, &path);
            break;
        }
        case Icon::Region:
            box(5, 5, 19, 19);
            box(2, 2, 7, 7);
            box(17, 2, 22, 7);
            box(2, 17, 7, 22);
            box(17, 17, 22, 22);
            break;
        case Icon::RoundedRectangle: {
            Gdiplus::GraphicsPath path;
            path.AddArc(3, 3, 7, 7, 180, 90);
            path.AddArc(14, 3, 7, 7, 270, 90);
            path.AddArc(14, 14, 7, 7, 0, 90);
            path.AddArc(3, 14, 7, 7, 90, 90);
            path.CloseFigure();
            graphics.DrawPath(&pen, &path);
            break;
        }
        case Icon::ChevronDown:
            line(6, 9, 12, 15);
            line(12, 15, 18, 9);
            break;
        case Icon::Minimize: line(5, 16, 19, 16); break;
        case Icon::Maximize: box(5, 5, 19, 19); break;
        case Icon::Scissors:
            circle(3, 15, 9, 21);
            circle(15, 15, 21, 21);
            line(8, 16, 19, 3);
            line(16, 16, 5, 3);
            break;
        case Icon::Help: {
            circle(3, 3, 21, 21);
            graphics.DrawBezier(&pen, 9, 10, 9, 5, 15, 5, 15, 10);
            graphics.DrawBezier(&pen, 15, 10, 15, 12, 12, 12, 12, 15);
            Gdiplus::SolidBrush dot(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
            graphics.FillEllipse(&dot, 11, 17, 2, 2);
            break;
        }
        case Icon::Pin:
            line(8, 3, 16, 3);
            line(9, 3, 9, 10);
            line(15, 3, 15, 10);
            line(9, 10, 6, 14);
            line(15, 10, 18, 14);
            line(6, 14, 18, 14);
            line(12, 14, 12, 21);
            break;
        case Icon::None: break;
    }
}

} // namespace easnap::ui
