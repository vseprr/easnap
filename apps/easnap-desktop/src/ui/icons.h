#pragma once

#include <windows.h>

namespace easnap::ui {

// Geometry icons shared by GDI native buttons. No font-dependent pictographs.
enum class Icon { None,
                  Record,
                  Pause,
                  Play,
                  Stop,
                  SystemAudio,
                  Microphone,
                  Cursor,
                  Copy,
                  Save,
                  Discard,
                  Appearance,
                  Sun,
                  Moon,
                  Pen,
                  Arrow,
                  Rectangle,
                  Ellipse,
                  Text,
                  Mosaic,
                  Undo,
                  Redo,
                  Clear,
                  Check,
                  Line,
                  Droplet,
                  Tag,
                  Spotlight,
                  Watermark,
                  TextFill,
                  TextOutline,
                  Hand,
                  Region,
                  RoundedRectangle,
                  ChevronDown,
                  Minimize,
                  Maximize,
                  Scissors,
                  Help,
                  Pin };
/// Draws `icon` as antialiased strokes on a 24-unit grid scaled to `rect`.
void DrawIcon(HDC dc, const RECT& rect, Icon icon, COLORREF color, float scale);

} // namespace easnap::ui
