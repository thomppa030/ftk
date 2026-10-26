#include "ftk/ui/kit/color_field.hpp"

#include "ftk/math/color_space.hpp"
#include "ftk/ui/kit/color_code.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/popup.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/text_field.hpp"

#include <glm/common.hpp>
#include <glm/vector_relational.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ftk::ui {

using namespace corekit;
using detail::from_hex;
using detail::to_hex;

namespace {

// The checks behind a colour's see-through part, as ImGui draws them.
constexpr Color CHECK_LIGHT{0.8f, 0.8f, 0.8f, 1.0f};
constexpr Color CHECK_DARK{0.5f, 0.5f, 0.5f, 1.0f};
constexpr Color WHITE{1.0f, 1.0f, 1.0f, 1.0f};
constexpr Color BLACK{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Color MID_GREY{0.5f, 0.5f, 0.5f, 1.0f};
// The picker a colour field opens is as wide as sixteen letters are tall,
// as ImGui's is.
constexpr float POPUP_PICKER_WIDTH = theme::BODY_TEXT * 16.0f;

// Hue, saturation and value from 0 to 1, and back, as ImGui reckons them.
glm::vec3 to_hsv(glm::vec3 rgb) {
    float k = 0.0f;
    if (rgb.g < rgb.b) {
        std::swap(rgb.g, rgb.b);
        k = -1.0f;
    }
    if (rgb.r < rgb.g) {
        std::swap(rgb.r, rgb.g);
        k = -2.0f / 6.0f - k;
    }
    const float chroma = rgb.r - std::min(rgb.g, rgb.b);
    return {std::fabs(k + (rgb.g - rgb.b) / (6.0f * chroma + 1e-20f)), chroma / (rgb.r + 1e-20f), rgb.r};
}

glm::vec3 to_rgb(glm::vec3 hsv) {
    const float v = hsv.z;
    if (hsv.y == 0.0f) return {v, v, v};
    const float h = std::fmod(hsv.x, 1.0f) * 6.0f;
    const int sector = static_cast<int>(h);
    const float f = h - static_cast<float>(sector);
    const float p = v * (1.0f - hsv.y);
    const float q = v * (1.0f - hsv.y * f);
    const float t = v * (1.0f - hsv.y * (1.0f - f));
    switch (sector) {
    case 0: return {v, t, p};
    case 1: return {q, v, p};
    case 2: return {p, v, t};
    case 3: return {p, q, v};
    case 4: return {t, p, v};
    default: return {v, p, q};
    }
}

int to_byte(float v) {
    return static_cast<int>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

// Checks `step` square over `rect`, its corners rounded as `corners` says.
void draw_checks(core::Rect rect, float step, float rounding, RoundCorners corners) {
    core::DrawList& dl = ui().draw_list();
    dl.rect_filled(rect.min, rect.max, faded(CHECK_LIGHT), rounding, corners);
    dl.push_clip(rect.min, rect.max);
    int row = 0;
    for (float y = rect.min.y; y < rect.max.y; y += step, ++row) {
        for (float x = rect.min.x + static_cast<float>(row % 2) * step; x < rect.max.x; x += step * 2.0f) {
            dl.rect_filled({x, y}, {std::min(x + step, rect.max.x), std::min(y + step, rect.max.y)}, faded(CHECK_DARK));
        }
    }
    dl.pop_clip();
}

// `colour` as a swatch at `rect`, its corners `rounding` round: opaque, or
// with `alpha_half` its right half over checks as its alpha leaves it, and
// `border` round it.
void fill_swatch(core::Rect rect, Color colour, bool alpha_half, bool border, float rounding) {
    core::DrawList& dl = ui().draw_list();
    const Color opaque{colour.r, colour.g, colour.b, 1.0f};
    if (alpha_half && colour.a < 1.0f) {
        const float mid = std::floor((rect.min.x + rect.max.x) * 0.5f);
        const core::Rect right{.min = {mid, rect.min.y}, .max = rect.max};
        draw_checks(right, std::min(rect.width(), rect.height()) / 2.99f, rounding, RoundCorners::Right);
        dl.rect_filled(right.min, right.max, faded(colour), rounding, RoundCorners::Right);
        dl.rect_filled(rect.min, {mid, rect.max.y}, faded(opaque), rounding, RoundCorners::Left);
    } else {
        dl.rect_filled(rect.min, rect.max, faded(opaque), rounding);
    }
    if (border) dl.rect(rect.min, rect.max, faded(theme::surface_sunken()), rounding);
}

// `rect` in bands a unit tall from top to bottom, each `shade(t)` with t
// its middle's way down, from 0 to 1, shading across from the first colour
// to the second: the core's gradients run across only. Each reaches a unit
// into the next, so no seam shows between them.
template <typename Shade>
void draw_bands(core::Rect rect, Shade&& shade) {
    core::DrawList& dl = ui().draw_list();
    const int bands = std::max(1, static_cast<int>(std::ceil(rect.height())));
    const float step = rect.height() / static_cast<float>(bands);
    for (int i = 0; i < bands; ++i) {
        const float top = rect.min.y + static_cast<float>(i) * step;
        const float bottom = std::min(top + step + 1.0f, rect.max.y);
        const auto [left, right] = shade((static_cast<float>(i) + 0.5f) / static_cast<float>(bands));
        dl.rect_gradient({rect.min.x, top}, {rect.max.x, bottom}, faded(left), faded(right));
    }
}

// The arrows that point into a bar from either side at `y`, as ImGui's.
void draw_bar_arrows(core::Rect bar, float y) {
    core::DrawList& dl = ui().draw_list();
    const float half = std::trunc(bar.width() * 0.2f);
    const float left = bar.min.x - 1.0f;
    const float right = bar.max.x + 1.0f;
    const auto pointing_right = [&](float tip, glm::vec2 size, Color colour) {
        dl.triangle_filled({tip - size.x, y - size.y}, {tip - size.x, y + size.y}, {tip, y}, faded(colour));
    };
    const auto pointing_left = [&](float tip, glm::vec2 size, Color colour) {
        dl.triangle_filled({tip + size.x, y + size.y}, {tip + size.x, y - size.y}, {tip, y}, faded(colour));
    };
    pointing_right(left + half + 2.0f, {half + 3.0f, half + 1.0f}, BLACK);
    pointing_right(left + half + 1.0f, {half + 1.0f, half}, WHITE);
    pointing_left(right - half - 2.0f, {half + 3.0f, half + 1.0f}, BLACK);
    pointing_left(right - half - 1.0f, {half + 1.0f, half}, WHITE);
}

// `count` numbers side by side across `width`, as a vector's fields share
// it: equal widths, the last taking what rounding leaves.
template <typename Draw>
Edit numbers_across(int count, float width, Draw&& draw_one, bool& started) {
    const float spacing = kit().style.item_inner_spacing.x;
    const float gaps = spacing * static_cast<float>(count - 1);
    const float one = std::max(1.0f, std::floor((width - gaps) / static_cast<float>(count)));
    const float last = std::max(1.0f, std::floor(width - (one + spacing) * static_cast<float>(count - 1)));
    Edit edit;
    for (int i = 0; i < count; ++i) {
        if (i > 0) same_line(spacing);
        IdScope component(i);
        next_width(i + 1 == count ? last : one);
        edit |= draw_one(i);
        started = started || item_activated();
    }
    return edit;
}

// Whether two colours are one as a byte each reads them: a linear colour
// comes back through sRGB a little off.
bool same_colour(glm::vec3 a, glm::vec3 b) {
    return glm::all(glm::lessThan(glm::abs(a - b), glm::vec3{0.5f / 255.0f}));
}

/// What the picker shows under its square.
enum class Below : uint8_t { Numbers, Hex };

// The picker at the cursor, `width` across, as ImGui lays it out: the square
// of saturation and value, the hue bar beside it and, with `alpha`, the
// alpha bar; then, as `below` says, the colour's numbers (red, green and
// blue, then hue, saturation and value) or its hex code. Edits sRGB
// `rgba`, `started` once a press or a field begins an edit.
Edit picker(Id id, float* rgba, bool alpha, float width, Below below, bool& started) {
    core::Context& context = ui();
    Kit& k = kit();
    const Style& style = k.style;
    const float bar = frame_height();
    const float gap = style.item_inner_spacing.x;
    const float bars = (alpha ? 2.0f : 1.0f) * (bar + gap);
    const float side = std::max(bar, width - bars);
    const core::Rect whole = context.layout().next({side + bars, side});
    const core::Rect square{.min = whole.min, .max = whole.min + glm::vec2{side}};
    const core::Rect hue_bar{.min = {square.max.x + gap, square.min.y},
                             .max = {square.max.x + gap + bar, square.max.y}};
    const core::Rect alpha_bar{.min = {hue_bar.max.x + gap, square.min.y},
                               .max = {hue_bar.max.x + gap + bar, square.max.y}};

    // Hue and saturation as the picker left them, while the colour is the
    // one it left.
    glm::vec3 rgb{rgba[0], rgba[1], rgba[2]};
    glm::vec3 hsv = k.picked.id == id && same_colour(k.picked.rgb, rgb) ? k.picked.hsv : to_hsv(rgb);

    Edit edit;
    bool hsv_changed = false;
    // A part dragged in: `set` takes where the pointer is in it, from 0 to 1
    // each way, and says whether that changed the colour.
    const auto drag = [&](const char* name, core::Rect rect, auto&& set) {
        const Id part = core::hash_label(name, id);
        const core::Interaction in = place(part, rect);
        if (in.pressed) {
            k.dragged = {.id = part};
            started = true;
        }
        if (in.held && k.dragged.id == part) {
            const glm::vec2 at = glm::clamp((in.position - rect.min) / rect.size(), 0.0f, 1.0f);
            if (set(at)) {
                edit.changed = true;
                k.dragged.edited = true;
            }
        }
        if (in.released && k.dragged.id == part) {
            edit.committed = edit.committed || k.dragged.edited;
            k.dragged = {};
        }
        return in;
    };
    const core::Interaction on_square = drag("##sv", square, [&](glm::vec2 at) {
        const glm::vec2 sv{at.x, 1.0f - at.y};
        if (sv == glm::vec2{hsv.y, hsv.z}) return false;
        hsv.y = sv.x;
        hsv.z = sv.y;
        hsv_changed = true;
        return true;
    });
    drag("##hue", hue_bar, [&](glm::vec2 at) {
        const float hue = std::min(at.y, 1.0f - 1e-4f);
        if (hue == hsv.x) return false;
        hsv.x = hue;
        hsv_changed = true;
        return true;
    });
    if (alpha) {
        drag("##alpha", alpha_bar, [&](glm::vec2 at) {
            const float a = 1.0f - at.y;
            if (a == rgba[3]) return false;
            rgba[3] = a;
            return true;
        });
    }
    if (hsv_changed) rgb = to_rgb(hsv);

    // The square: grey to the hue across, bright to black down.
    core::DrawList& dl = context.draw_list();
    const glm::vec3 pure = to_rgb({hsv.x, 1.0f, 1.0f});
    draw_bands(square, [&](float t) {
        const float v = 1.0f - t;
        return std::pair{Color{v, v, v, 1.0f}, Color{pure * v, 1.0f}};
    });
    draw_bands(hue_bar, [&](float t) {
        const Color colour{to_rgb({t, 1.0f, 1.0f}), 1.0f};
        return std::pair{colour, colour};
    });
    draw_bar_arrows(hue_bar, std::round(hue_bar.min.y + hsv.x * side));
    if (alpha) {
        draw_checks(alpha_bar, alpha_bar.width() * 0.5f, 0.0f, RoundCorners::All);
        draw_bands(alpha_bar, [&](float t) {
            const Color colour{rgb, 1.0f - t};
            return std::pair{colour, colour};
        });
        draw_bar_arrows(alpha_bar, std::round(alpha_bar.min.y + (1.0f - rgba[3]) * side));
    }
    // Where the colour is in the square, a ring a little bigger while held.
    const glm::vec2 cursor = glm::clamp(glm::round(square.min + glm::vec2{hsv.y, 1.0f - hsv.z} * side),
                                        square.min + 2.0f, square.max - 2.0f);
    const float ring = side * 0.08f * (on_square.held ? 0.55f : 0.40f);
    dl.circle_filled(cursor, ring, faded(Color{rgb, 1.0f}));
    dl.circle(cursor, ring + 1.0f, faded(MID_GREY));
    dl.circle(cursor, ring, faded(WHITE));

    if (below == Below::Numbers) {
        // Red, green and blue (and alpha) as bytes, then hue in degrees and
        // saturation and value in percent.
        int bytes[4]{to_byte(rgb.r), to_byte(rgb.g), to_byte(rgb.b), to_byte(rgba[3])};
        const char* const letters[4]{"R", "G", "B", "A"};
        const Color colours[4]{theme::axis_x(), theme::axis_y(), theme::axis_z(), theme::text_secondary()};
        Edit numbers;
        {
            IdScope row("##rgb");
            numbers = numbers_across(
                alpha ? 4 : 3, whole.width(),
                [&](int i) { return lettered_int("##n", bytes[i], 0, 255, letters[i], colours[i]); }, started);
        }
        if (numbers.changed) {
            rgb = glm::vec3{bytes[0], bytes[1], bytes[2]} / 255.0f;
            if (alpha) rgba[3] = static_cast<float>(bytes[3]) / 255.0f;
            hsv = to_hsv(rgb);
        }
        int hsv_whole[3]{static_cast<int>(std::lround(hsv.x * 360.0f)), static_cast<int>(std::lround(hsv.y * 100.0f)),
                         static_cast<int>(std::lround(hsv.z * 100.0f))};
        const char* const hsv_letters[3]{"H", "S", "V"};
        const int hsv_max[3]{360, 100, 100};
        const Unit hsv_units[3]{Unit::Degrees, Unit::Percent, Unit::Percent};
        Edit hsv_numbers;
        {
            IdScope row("##hsv");
            hsv_numbers = numbers_across(
                3, whole.width(),
                [&](int i) {
                    return lettered_int("##n", hsv_whole[i], 0, hsv_max[i], hsv_letters[i], theme::text_secondary(),
                                        hsv_units[i]);
                },
                started);
        }
        if (hsv_numbers.changed) {
            hsv = glm::vec3{hsv_whole[0], hsv_whole[1], hsv_whole[2]} / glm::vec3{360.0f, 100.0f, 100.0f};
            rgb = to_rgb(hsv);
        }
        edit |= numbers;
        edit |= hsv_numbers;
    } else {
        const float now[3]{rgb.r, rgb.g, rgb.b};
        std::string hex = to_hex(now, false);
        next_width(whole.width());
        if (text_field("##hex", hex).committed) {
            float typed[4]{rgb.r, rgb.g, rgb.b, 1.0f};
            if (from_hex(hex, typed, false)) {
                rgb = {typed[0], typed[1], typed[2]};
                hsv = to_hsv(rgb);
                edit |= Edit{true, true};
            }
        }
        started = started || item_activated();
    }

    rgba[0] = rgb.r;
    rgba[1] = rgb.g;
    rgba[2] = rgb.b;
    k.picked = {.id = id, .rgb = rgb, .hsv = hsv};
    return edit;
}

// The field over sRGB components, with or without alpha.
Edit srgb_field(const char* id, float* rgba, bool alpha) {
    core::Context& context = ui();
    const float side = frame_height();
    const float spacing = kit().style.item_inner_spacing.x;
    const float width = item_width();

    const std::string before = to_hex(rgba, alpha);
    const Id field_id = context.id(id);
    bool started = false;
    Edit edit;
    {
        IdScope scope(id);
        const core::Rect swatch_rect = context.layout().next({side, side});
        const Id swatch_id = context.id("##swatch");
        const Id popup = context.id("##picker");
        if (place(swatch_id, swatch_rect).clicked) context.open_popup(popup);
        context.semantics(swatch_id, core::Role::Button, shown(id), before);
        fill_swatch(swatch_rect, {rgba[0], rgba[1], rgba[2], alpha ? rgba[3] : 1.0f}, alpha, true,
                    kit().style.frame_rounding);

        same_line(spacing);
        next_width(std::max(width - side - spacing, 1.0f));
        std::string hex = before;
        {
            // The code in the smaller mono face.
            FontScope mono(Font::Mono, theme::SMALL_TEXT);
            if (text_field("##hex", hex).committed && from_hex(hex, rgba, alpha)) edit = {true, true};
        }

        if (begin_popup(popup, {.padding = kit().style.window_padding})) {
            edit |= picker(context.id("##pick"), rgba, alpha, POPUP_PICKER_WIDTH, Below::Numbers, started);
            end_popup();
        }
    }
    // Recorded as the colour's code, over the hex field's own record.
    detail::track_field(field_id, before, to_hex(rgba, alpha), started, edit);
    return edit;
}

} // namespace

Edit corekit::popup_picker(Id id, float* rgba, bool alpha) {
    bool started = false;
    return picker(id, rgba, alpha, POPUP_PICKER_WIDTH, Below::Numbers, started);
}

Edit color(const char* id, glm::vec3& value, ColorSpace space) {
    const glm::vec3 shown = space == ColorSpace::Linear ? color_space::linear_to_srgb(value) : value;
    float rgba[4]{shown.r, shown.g, shown.b, 1.0f};
    const Edit edit = srgb_field(id, rgba, false);
    if (edit.changed) {
        const glm::vec3 srgb{rgba[0], rgba[1], rgba[2]};
        value = space == ColorSpace::Linear ? color_space::srgb_to_linear(srgb) : srgb;
    }
    return edit;
}

Edit color(const char* id, glm::vec4& value, ColorSpace space) {
    const glm::vec4 shown = space == ColorSpace::Linear ? color_space::linear_to_srgb(value) : value;
    float rgba[4]{shown.r, shown.g, shown.b, shown.a};
    const Edit edit = srgb_field(id, rgba, true);
    if (edit.changed) {
        const glm::vec4 srgb{rgba[0], rgba[1], rgba[2], rgba[3]};
        value = space == ColorSpace::Linear ? color_space::srgb_to_linear(srgb) : srgb;
    }
    return edit;
}

bool swatch(const char* id, glm::vec4 colour, glm::vec2 size) {
    core::Context& context = ui();
    const float side = frame_height();
    const core::Rect rect = context.layout().next({size.x > 0.0f ? size.x : side, size.y > 0.0f ? size.y : side});
    const Id swatch_id = context.id(id);
    const bool clicked = place(swatch_id, rect).clicked;
    context.semantics(swatch_id, core::Role::Button, shown(id));
    fill_swatch(rect, colour, false, false, kit().style.frame_rounding);
    return clicked;
}

void draw_swatch(glm::vec2 min, glm::vec2 max, glm::vec4 colour, bool alpha) {
    const core::Rect rect{.min = min, .max = max};
    // A small one keeps the field's shape: corners no rounder than a fifth
    // of its height.
    fill_swatch(rect, colour, alpha, true, std::min(kit().style.frame_rounding, rect.height() * 0.2f));
}

Edit color_popup(const char* id, glm::vec4& srgb, bool alpha) {
    Edit edit;
    if (const Popup popup(id); popup) {
        float rgba[4]{srgb.r, srgb.g, srgb.b, alpha ? srgb.a : 1.0f};
        edit = popup_picker(ui().id("##pick"), rgba, alpha);
        if (edit.changed) srgb = {rgba[0], rgba[1], rgba[2], alpha ? rgba[3] : srgb.a};
    }
    return edit;
}

Edit color_picker(const char* id, glm::vec3& value) {
    float rgba[4]{value.r, value.g, value.b, 1.0f};
    const std::string before = to_hex(rgba, false);
    const Id field_id = ui().id(id);
    bool started = false;
    Edit edit;
    {
        // One item, the whole picker, as ImGui's group of it is.
        Group whole;
        IdScope scope(id);
        edit = picker(field_id, rgba, false, item_width(), Below::Hex, started);
    }
    if (edit.changed) value = {rgba[0], rgba[1], rgba[2]};
    detail::track_field(field_id, before, to_hex(rgba, false), started, edit);
    return edit;
}

} // namespace ftk::ui
