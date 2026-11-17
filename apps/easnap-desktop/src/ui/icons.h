#pragma once

#include <windows.h>

#include <cstddef>

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
/// Number of Icon values, None included: the size of the icon painter table.
inline constexpr std::size_t kIconCount = static_cast<std::size_t>(Icon::Pin) + 1;
/// Draws `icon` as antialiased strokes on a 24-unit grid scaled to `rect`.
void DrawIcon(HDC dc, const RECT& rect, Icon icon, COLORREF color);

} // namespace easnap::ui
