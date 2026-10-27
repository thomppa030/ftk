#include "ftk/ui/kit/slot.hpp"

#include "ftk/ui/kit/asset_kind.hpp"
#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/feedback.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"
#include "ftk/ui/theme.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float FIELD_PICTURE = 18.0f;

} // namespace

void draw_slot_face(glm::vec2 min, glm::vec2 max, const SlotFace& face) {
    const Style& style = kit().style;
    const float height = max.y - min.y;
    core::DrawList& dl = ui().draw_list();
    const float rounding = style.frame_rounding;
    const FrameColours frame = frame_colours();
    dl.rect_filled(min, max, faded(face.hovered ? frame.hover : frame.rest), rounding);
    if (face.drop == SlotDrop::fits) {
        dl.rect_filled(min, max, faded(theme::drop_fits()), rounding);
        dl.rect(min, max, faded(theme::accent()), rounding);
    } else if (face.drop == SlotDrop::refused) {
        dl.rect(min, max, faded(theme::error()), rounding);
    }

    const float text_y = min.y + style.frame_padding.y;
    float x = min.x + style.frame_padding.x;
    if (face.picture) {
        const float top = min.y + (height - FIELD_PICTURE) * 0.5f;
        dl.image(face.picture, {x, top}, {x + FIELD_PICTURE, top + FIELD_PICTURE});
        x += FIELD_PICTURE + theme::GAP_S + theme::GAP_XS;
    } else if (face.glyph != nullptr) {
        draw_text({x, text_y}, face.glyph_colour, face.glyph);
        x += measure(face.glyph).x + theme::GAP_S + theme::GAP_XS;
    }

    // Without a chevron the text runs to the field's padding.
    const float chevron = face.chevron ? height : style.frame_padding.x;
    dl.push_clip({x, min.y}, {max.x - chevron, max.y});
    draw_text({x, text_y}, face.text_colour, face.text);
    dl.pop_clip();
    if (!face.chevron) return;
    draw_text({max.x - chevron + (chevron - measure(icon::dropdown).x) * 0.5f, text_y}, theme::text_secondary(),
              icon::dropdown);
}

void refusal_tooltip(std::string_view why) {
    // The refused icon in the error colour, then why, beside the drag.
    const std::string text = std::string(icon::refused) + "  " + std::string(why);
    TextColour error(theme::error());
    tooltip(text);
}

Slot::Slot(const char* payload, const Judge& judge) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const float height = frame_height();
    const float width = item_width();
    // The clear button is always there, disabled while the slot is empty,
    // so the field keeps its width whether it holds something or not.
    const float clear_w = height + style.item_inner_spacing.x;
    min_ = cursor();
    max_ = {min_.x + std::max(width - clear_w, height), min_.y + height};

    // The field is one button: clicking anywhere on it opens the picker.
    const core::Rect rect = context.layout().next(max_ - min_);
    const core::Interaction in = place(context.id("##slot"), rect);
    clicked_ = in.clicked;
    hovered_ = in.hovered;

    // Something dragged is judged while it is still over the field: what
    // fits lights the field, what doesn't outlines it red and says why, and
    // is not taken.
    if (payload == nullptr) return;
    const auto drop = context.drop_target(payload);
    if (!drop) return;
    const std::string refusal = judge ? judge(drop->payload) : std::string();
    if (!refusal.empty()) {
        drop_ = SlotDrop::refused;
        refusal_tooltip(refusal);
        return;
    }
    drop_ = SlotDrop::fits;
    if (drop->delivered) {
        const auto* bytes = static_cast<const char*>(drop->payload.data);
        dropped_data_.assign(bytes, bytes + drop->payload.size);
        dropped_ = {dropped_data_.data(), dropped_data_.size()};
        has_drop_ = true;
    }
}

bool Slot::finish(SlotFace face, bool empty, std::string_view problem, std::string_view kept) {
    face.drop = drop_;
    face.hovered = hovered_;
    draw_slot_face(min_, max_, face);

    same_line(kit().style.item_inner_spacing.x);
    bool cleared = false;
    {
        Disabled off(empty || !kept.empty());
        const std::string tooltip = !kept.empty() ? std::string(kept) : empty ? "Nothing to clear" : "Clear";
        cleared = icon_button("##clear", icon::clear, tooltip.c_str(), ButtonKind::GhostDanger);
    }
    if (!problem.empty()) status(Severity::Error, std::string(problem).c_str());
    return cleared;
}

SlotFace asset_face(std::string_view name, AssetPresence presence, std::string_view slot_extension,
                    std::string& text) {
    SlotFace face;
    switch (presence) {
        case AssetPresence::empty:
            face.glyph = asset_kind(slot_extension).icon;
            face.glyph_colour = theme::text_disabled();
            text = "None";
            face.text_colour = theme::text_secondary();
            break;
        case AssetPresence::missing:
            face.glyph = icon::warning;
            face.glyph_colour = theme::error();
            text = "missing: " + std::string(name);
            face.text_colour = theme::error();
            break;
        case AssetPresence::held:
        case AssetPresence::built_in: {
            // A built-in has no file name to tell its kind by: it is what the slot takes.
            const AssetKind& kind = asset_kind(presence == AssetPresence::built_in
                                                   ? slot_extension
                                                   : std::filesystem::path(name).extension().string());
            face.glyph = kind.icon;
            face.glyph_colour = kind_colour(kind);
            text = name;
            face.text_colour = theme::text();
            break;
        }
    }
    face.text = text;
    return face;
}

} // namespace ftk::ui
