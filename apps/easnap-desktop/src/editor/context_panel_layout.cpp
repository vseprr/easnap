// Easnap - the layout of the screenshot editor's property island.
#include "editor/context_panel_layout.h"

#include <algorithm>
#include <initializer_list>
#include <string>

#include "editor/annotation_tools.h"
#include "strings/strings.h"
#include "ui/control_ids.h"

namespace easnap::editor {
namespace {

/// The width in pixels of `text` on one line in `font`, as a static label draws it.
float TextPixelWidth(HFONT font, const std::wstring& text) {
    float width = 0;
    if (HDC dc = ::CreateCompatibleDC(nullptr)) {
        const HGDIOBJ previous = font != nullptr ? ::SelectObject(dc, font) : nullptr;
        RECT box{};
        ::DrawTextW(dc, text.c_str(), -1, &box, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        width = static_cast<float>(box.right - box.left);
        if (previous != nullptr) { ::SelectObject(dc, previous); }
        ::DeleteDC(dc);
    }
    return width;
}

/// Lays one tool's property island out, wrapped in the width of the tools
/// island, into the item, field and separator lists it is given.
class ContextLayout {
public:
    ContextLayout(float available_width, float scale, HFONT font, std::vector<ToolbarItem>& items,
                  std::vector<std::pair<int, RECT>>& fields, std::vector<RECT>& separators)
        : flow_(available_width, scale), font_(font), items_(items), fields_(fields), separators_(separators) {}

    /// A control `width` DIPs wide.
    void Field(int id, float width) {
        auto r = flow_.Place(width * flow_.s);
        widest_ = std::max(widest_, r.right + flow_.pad);
        fields_.push_back({id, {static_cast<LONG>(r.left), static_cast<LONG>(r.top), static_cast<LONG>(r.right), static_cast<LONG>(r.bottom)}});
    }
    /// A value label as wide as the widest text it can show, and never
    /// narrower than its design width. A font's pixel height does not follow
    /// the scale exactly, so a fixed width that fits at 125 % cut
    /// "Boyut 96 pt" at 150 %.
    void ValueField(int id, float minimum, std::initializer_list<std::wstring> texts) {
        float text_width = 0;
        for (const std::wstring& value : texts) { text_width = std::max(text_width, TextPixelWidth(font_, value)); }
        Field(id, std::max(minimum, (text_width + 1.0f) / flow_.s));
    }
    /// A separator line between two groups.
    void Separator() { separators_.push_back(flow_.Separator(8 * flow_.s)); }
    /// An option button, placed only while `shown`.
    void Option(int id, bool shown, float width = kToolButton) {
        ToolbarItem item;
        item.kind = ToolbarItem::Kind::Option;
        item.identifier = id;
        item.label = item.tooltip = strings::Get(ButtonLook(id).tooltip);
        item.shown = shown;
        if (shown) {
            item.rect = flow_.Place(width * flow_.s);
            widest_ = std::max(widest_, item.rect.right + flow_.pad);
        }
        items_.push_back(item);
    }
    /// The thickness buttons, placed only while `shown`.
    void Thicknesses(bool shown) {
        const float s = flow_.s;
        for (size_t i = 0; i < kThicknessOptions.size(); ++i) {
            ToolbarItem item;
            item.kind = ToolbarItem::Kind::Thickness;
            item.thickness = kThicknessOptions[i];
            item.identifier = kThicknessButtons.Id(static_cast<int>(i));
            item.shown = shown;
            if (item.shown) {
                item.rect = flow_.Place(kToolButton * s);
                widest_ = std::max(widest_, item.rect.right + flow_.pad);
            }
            items_.push_back(item);
        }
    }
    /// The colour swatches, placed only while `shown`.
    void Colors(bool shown) {
        const float s = flow_.s;
        for (size_t i = 0; i < kAnnotationSwatchColors.size(); ++i) {
            ToolbarItem item;
            item.kind = ToolbarItem::Kind::Color;
            item.color = kAnnotationSwatchColors[i];
            item.identifier = kColorButtons.Id(static_cast<int>(i));
            item.shown = shown;
            if (item.shown) {
                item.rect = flow_.Place(kSwatch * s);
                widest_ = std::max(widest_, item.rect.right + flow_.pad);
            }
            items_.push_back(item);
        }
    }
    /// The brush sizes, in one row of the pop-up below its callout.
    void BrushSizes() {
        const float s = flow_.s, pad = flow_.pad, row = flow_.row;
        for (size_t i = 0; i < kBrushSizeOptions.size(); ++i) {
            ToolbarItem item;
            item.kind = ToolbarItem::Kind::BrushSize;
            item.brush_size = kBrushSizeOptions[i];
            item.identifier = kBrushSizeButtons.Id(static_cast<int>(i));
            item.shown = true;
            const float left = pad + static_cast<float>(i) * (kToolButton + kItemGap) * s;
            const float callout = 6 * s;
            item.rect = {left, callout + (row - kToolButton * s) / 2, left + kToolButton * s, callout + (row + kToolButton * s) / 2};
            items_.push_back(item);
        }
    }
    /// The island's height with `island`, else 0, and its widest row.
    [[nodiscard]] std::pair<float, float> Extent(bool island) const {
        return {island ? flow_.y + flow_.row : 0.0f, widest_};
    }

private:
    ToolbarFlow flow_;
    HFONT font_;
    std::vector<ToolbarItem>& items_;
    std::vector<std::pair<int, RECT>>& fields_;
    std::vector<RECT>& separators_;
    float widest_{0};
};

}  // namespace

std::pair<float, float> LayOutContextPanel(ToolId tool, float available_width, float scale, HFONT font, std::vector<ToolbarItem>& items,
                                           std::vector<std::pair<int, RECT>>& fields, std::vector<RECT>& separators) {
    ContextLayout layout(available_width, scale, font, items, fields, separators);
    const ToolProperties& shows = PropertiesOf(tool);
    layout.Option(kTextPlainButton, shows.text_style);
    layout.Option(kTextFilledButton, shows.text_style);
    layout.Option(kTextOutlinedButton, shows.text_style);
    layout.Option(kPixelateButton, shows.effect);
    layout.Option(kBlurButton, shows.effect);
    if (shows.effect) { layout.Separator(); }
    layout.Option(kBrushMethodButton, shows.effect, 48);
    layout.Option(kRegionMethodButton, shows.effect);
    if (shows.effect) {
        layout.Separator();
        layout.ValueField(kEffectStrengthValue, 125, {strings::Format(IDS_EDITOR_BLUR_INTENSITY_VALUE, {L"100"}), strings::Format(IDS_EDITOR_BLOCK_SIZE_VALUE, {L"72"})});
        layout.Field(kEffectStrengthSlider, 155);
    }
    layout.Option(kSpotlightRectangleButton, shows.spotlight);
    layout.Option(kSpotlightEllipseButton, shows.spotlight);
    layout.Option(kSpotlightRoundedButton, shows.spotlight);
    if (shows.spotlight) {
        layout.Separator();
        layout.ValueField(kOpacityValue, 110, {strings::Format(IDS_EDITOR_OPACITY_VALUE, {L"100"})});
        layout.Field(kOpacitySlider, 155);
    }
    if (shows.font_size) {
        layout.Field(kFontSizeCombo, 96);
        layout.Separator();
    }
    if (shows.watermark) {
        layout.Field(kWatermarkTextEdit, 140);
        layout.Separator();
    }
    layout.Thicknesses(shows.thickness);
    if (shows.thickness) { layout.Separator(); }
    layout.Colors(shows.colors);
    if (shows.watermark) {
        layout.Separator();
        layout.ValueField(kOpacityValue, 85, {strings::Format(IDS_EDITOR_OPACITY_VALUE, {L"100"})});
        layout.Field(kOpacitySlider, 100);
        layout.Separator();
        layout.ValueField(kFontSizeValue, 65, {strings::Format(IDS_EDITOR_SIZE_VALUE, {L"96"})});
        layout.Field(kFontSizeSlider, 100);
    }
    layout.BrushSizes();
    return layout.Extent(shows.island);
}


}  // namespace easnap::editor
