#include "ftk/ui/kit/field.hpp"

#include "ftk/ui/kit/button.hpp"
#include "ftk/ui/kit/core/kit.hpp"
#include "ftk/ui/kit/core/text_input.hpp"
#include "ftk/ui/kit/edit_record.hpp"
#include "ftk/ui/kit/icons.hpp"
#include "ftk/ui/kit/item.hpp"
#include "ftk/ui/kit/layout.hpp"
#include "ftk/ui/kit/scope.hpp"
#include "ftk/ui/kit/text.hpp"
#include "ftk/ui/kit/tooltip.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

namespace ftk::ui {

using namespace corekit;

namespace {

constexpr float AXIS_LETTER_SIZE = 11.5f;
// The slider's grab: its narrowest, and its inset from the frame.
constexpr float GRAB_MIN = 8.0f;
constexpr float GRAB_INSET = 2.0f;
constexpr float GRAB_ROUNDING = 2.0f;

struct Axis {
    const char* letter;
    Color colour;
};

// Whether a field's range lets its value go below zero. Such a field keeps
// room for the minus sign even while the value is positive, so the digits
// stay still when the value crosses zero.
bool can_be_negative(double min, double max) {
    return min >= max || min < 0.0;
}

std::string formatted(const char* format, double value) {
    char number[64];
    std::snprintf(number, sizeof(number), format, value);
    return number;
}

// The field's value as its edit record reads it: the number and its unit.
std::string value_text(const char* format, double value, Unit unit) {
    return detail::with_unit(formatted(format, value).c_str(), unit_symbol(unit));
}

// The number the field shows, and keeps: rounded as it is written.
double rounded(const char* format, double value) {
    return std::strtod(formatted(format, value).c_str(), nullptr);
}

// The number and its unit drawn over the field's frame, centred, the unit
// after it in the secondary colour and an axis letter pinned to its left.
void draw_number(core::Rect rect, std::string_view number, Unit unit, const Axis* axis, bool signed_range) {
    core::Context& context = ui();
    const Style& style = kit().style;
    const char* symbol = unit_symbol(unit);
    const bool negative = !number.empty() && number.front() == '-';
    const std::string_view digits = negative ? number.substr(1) : number;
    const float sign_w = negative || signed_range ? measure("-").x : 0.0f;
    const float number_w = sign_w + measure(digits).x;
    const float unit_w = *symbol != '\0' ? theme::GAP_S + measure(symbol).x : 0.0f;
    const float left = rect.min.x + style.frame_padding.x;
    const float size = text_style().size;
    const float y = std::floor((rect.min.y + rect.max.y - size) * 0.5f);

    core::DrawList& dl = context.draw_list();
    dl.push_clip(rect.min, rect.max);
    float text_left = left;
    if (axis != nullptr) {
        // The smaller bold letter sits on the number's baseline.
        const float baseline = context.text().baseline(text_style());
        FontScope bold(Font::Bold, AXIS_LETTER_SIZE);
        const float letter_baseline = context.text().baseline(text_style());
        draw_text({left, y + std::floor(baseline - letter_baseline)}, axis->colour, axis->letter);
        text_left += measure(axis->letter).x + theme::GAP_S + theme::GAP_XS;
    }
    float x = std::floor((rect.min.x + rect.max.x - number_w - unit_w) * 0.5f);
    x = std::max(x, text_left);
    if (negative) draw_text({x, y}, corekit::text_colour(), "-");
    draw_text({x + sign_w, y}, corekit::text_colour(), digits);
    if (*symbol != '\0') draw_text({x + number_w + theme::GAP_S, y}, theme::text_secondary(), symbol);
    dl.pop_clip();
}

enum class Kind : uint8_t { Drag, Slider };

struct Number {
    Kind kind{Kind::Drag};
    double speed{0.1};
    double min{0.0};
    double max{0.0};
    const char* format{"%.2f"};
    bool integer{false};
    bool logarithmic{false};
    Unit unit{Unit::None};
    const Axis* axis{nullptr};
    /// Shown in place of the number: a slider's own words for its value.
    const char* text{nullptr};
};

double clamped(const Number& spec, double value) {
    if (spec.min < spec.max) value = std::clamp(value, spec.min, spec.max);
    if (spec.integer) return std::round(value);
    return rounded(spec.format, value);
}

// Where along the slider `value` is, from 0 to 1, and back.
double slider_t(const Number& spec, double value) {
    if (spec.max <= spec.min) return 0.0;
    if (spec.logarithmic && spec.min > 0.0) return std::log(value / spec.min) / std::log(spec.max / spec.min);
    return (value - spec.min) / (spec.max - spec.min);
}

double slider_value(const Number& spec, double t) {
    t = std::clamp(t, 0.0, 1.0);
    if (spec.logarithmic && spec.min > 0.0) return spec.min * std::pow(spec.max / spec.min, t);
    return spec.min + (spec.max - spec.min) * t;
}

// A number field at the cursor, as wide as the next item: dragged across,
// slid, or typed after a click with Ctrl, as ImGui's are.
Edit number_field(const char* id, double& value, const Number& spec) {
    core::Context& context = ui();
    Kit& k = kit();
    const core::Rect rect = context.layout().next({item_width(), frame_height()});
    const Id field_id = context.id(id);
    const Id typed_id = core::hash_label("#TYPED", field_id);
    Edit edit;

    // Being typed: a text box over the frame until it lets the keyboard go.
    if (k.typed.id == field_id) {
        const TextInput typed = text_input(typed_id, rect, k.typed.text, {.select_all = true});
        if (typed.deactivated) {
            const std::string text = std::exchange(k.typed, {}).text;
            char* end = nullptr;
            const double parsed = std::strtod(text.c_str(), &end);
            if (!typed.cancelled && end != text.c_str()) {
                const double next = clamped(spec, parsed);
                if (next != value) {
                    value = next;
                    edit = {true, true};
                }
            }
        }
        k.item.edited = edit.committed;
        return edit;
    }

    const core::Interaction in = place(field_id, rect);
    // A drag field types on a double click too; a slider only with Ctrl.
    if ((in.pressed && context.modifiers().ctrl) || (in.double_clicked && spec.kind == Kind::Drag)) {
        k.typed = {.id = field_id, .text = formatted(spec.integer ? "%.0f" : spec.format, value)};
        context.set_focus(typed_id);
        if (k.dragged.id == field_id) k.dragged = {};
    } else {
        if (in.pressed) k.dragged = {.id = field_id, .start = value};
        if (in.held && k.dragged.id == field_id) {
            double next = value;
            if (spec.kind == Kind::Drag) {
                next = k.dragged.start + static_cast<double>(in.position.x - in.press_position.x) * spec.speed;
            } else {
                const double grab = spec.integer && spec.max > spec.min
                                        ? std::max(static_cast<double>(rect.width() - 2.0f * GRAB_INSET) /
                                                       (spec.max - spec.min + 1.0),
                                                   static_cast<double>(GRAB_MIN))
                                        : static_cast<double>(GRAB_MIN);
                const double left = rect.min.x + GRAB_INSET + grab * 0.5;
                const double usable = std::max(static_cast<double>(rect.width()) - 2.0 * GRAB_INSET - grab, 1.0);
                next = slider_value(spec, (static_cast<double>(in.position.x) - left) / usable);
            }
            next = clamped(spec, next);
            if (next != value) {
                value = next;
                edit.changed = true;
                k.dragged.edited = true;
            }
        }
        if (in.released && k.dragged.id == field_id) {
            edit.committed = k.dragged.edited;
            k.dragged = {};
        }
    }

    draw_frame(rect, in, false);
    if (spec.kind == Kind::Slider && spec.max > spec.min) {
        const float grab = spec.integer ? std::max((rect.width() - 2.0f * GRAB_INSET) /
                                                       static_cast<float>(spec.max - spec.min + 1.0),
                                                   GRAB_MIN)
                                        : GRAB_MIN;
        const float t = static_cast<float>(std::clamp(slider_t(spec, value), 0.0, 1.0));
        const float x = rect.min.x + GRAB_INSET + (rect.width() - 2.0f * GRAB_INSET - grab) * t;
        const Color colour = in.held ? theme::accent_hov() : theme::accent();
        context.draw_list().rect_filled({x, rect.min.y + GRAB_INSET}, {x + grab, rect.max.y - GRAB_INSET},
                                        faded(colour), GRAB_ROUNDING);
    }
    const std::string number = spec.text != nullptr ? spec.text : formatted(spec.integer ? "%.0f" : spec.format, value);
    draw_number(rect, number, spec.text != nullptr ? Unit::None : spec.unit, spec.axis,
                spec.text == nullptr && can_be_negative(spec.min, spec.max));
    context.semantics(field_id, core::Role::Slider, shown(id), number);
    k.item.edited = edit.committed;
    return edit;
}

// Records a single field's finished edit.
void track(const std::string& before, const std::string& after, const Edit& edit) {
    detail::track_field(kit().item.id, before, after, item_activated(), edit);
}

Edit drag_one(const char* id, float& value, const DragSpec& spec, const Axis* axis) {
    const std::string before = value_text(spec.format, value, spec.unit);
    double number = value;
    const Edit edit = number_field(id, number,
                                   {.kind = Kind::Drag,
                                    .speed = spec.speed,
                                    .min = spec.min,
                                    .max = spec.max,
                                    .format = spec.format,
                                    .unit = spec.unit,
                                    .axis = axis});
    value = static_cast<float>(number);
    track(before, value_text(spec.format, value, spec.unit), edit);
    return edit;
}

Edit drag_int_axis(const char* id, int& value, float speed, int min, int max, Unit unit, const Axis* axis) {
    const std::string before = value_text("%.0f", value, unit);
    double number = value;
    const Edit edit = number_field(
        id, number,
        {.kind = Kind::Drag, .speed = speed, .min = static_cast<double>(min), .max = static_cast<double>(max),
         .format = "%.0f", .integer = true, .unit = unit, .axis = axis});
    value = static_cast<int>(number);
    track(before, value_text("%.0f", value, unit), edit);
    return edit;
}

// Several numbers as one value, "(1.00, 0.00, 2.00) m".
template <typename T, typename Format>
std::string vector_text(const T* values, int count, Unit unit, Format&& format) {
    std::string text = "(";
    for (int i = 0; i < count; ++i) {
        if (i > 0) text += ", ";
        text += format(values[i]);
    }
    text += ")";
    return detail::with_unit(text.c_str(), unit_symbol(unit));
}

// `count` fields side by side across the next item's width, as ImGui
// shares it: equal widths, the last taking what rounding leaves.
template <typename Draw>
Edit fields_across(const char* id, int count, Draw&& draw_one, bool& started) {
    const float spacing = kit().style.item_inner_spacing.x;
    const float full = item_width();
    const float one = std::max(1.0f, std::floor((full - spacing * static_cast<float>(count - 1)) / static_cast<float>(count)));
    const float last = std::max(1.0f, std::floor(full - (one + spacing) * static_cast<float>(count - 1)));
    Edit edit;
    Group group;
    IdScope scope(id);
    for (int i = 0; i < count; ++i) {
        if (i > 0) same_line(spacing);
        IdScope component(i);
        next_width(i + 1 == count ? last : one);
        edit |= draw_one(i);
        started = started || item_activated();
    }
    return edit;
}

Edit vec_n(const char* id, float* values, int count, const DragSpec& spec) {
    // A fourth component has no gizmo axis; its letter is plain.
    const Axis axes[] = {
        {"X", theme::axis_x()}, {"Y", theme::axis_y()}, {"Z", theme::axis_z()}, {"W", theme::text_secondary()},
    };
    auto number = [&](float v) { return value_text(spec.format, v, Unit::None); };
    const std::string before = vector_text(values, count, spec.unit, number);
    const Id field_id = ui().id(id);
    bool started = false;
    const Edit edit = fields_across(
        id, count, [&](int i) { return drag_one("##v", values[i], spec, &axes[i]); }, started);
    // The record names the whole vector, over the one number it replaces.
    detail::track_field(field_id, before, vector_text(values, count, spec.unit, number), started, edit);
    return edit;
}

} // namespace

Edit drag(const char* id, float& value, const DragSpec& spec) {
    return drag_one(id, value, spec, nullptr);
}

Edit drag_int(const char* id, int& value, float speed, int min, int max, Unit unit) {
    return drag_int_axis(id, value, speed, min, max, unit, nullptr);
}

Edit slider(const char* id, float& value, float min, float max, Unit unit, const char* format, bool logarithmic) {
    const std::string before = value_text(format, value, unit);
    double number = value;
    const Edit edit = number_field(id, number,
                                   {.kind = Kind::Slider,
                                    .min = min,
                                    .max = max,
                                    .format = format,
                                    .logarithmic = logarithmic,
                                    .unit = unit});
    value = static_cast<float>(number);
    track(before, value_text(format, value, unit), edit);
    return edit;
}

Edit slider_int(const char* id, int& value, int min, int max, Unit unit) {
    const std::string before = value_text("%.0f", value, unit);
    double number = value;
    const Edit edit = number_field(id, number,
                                   {.kind = Kind::Slider,
                                    .min = static_cast<double>(min),
                                    .max = static_cast<double>(max),
                                    .format = "%.0f",
                                    .integer = true,
                                    .unit = unit});
    value = static_cast<int>(number);
    track(before, value_text("%.0f", value, unit), edit);
    return edit;
}

Edit slider_labelled(const char* id, float& value, float min, float max, const char* text) {
    double number = value;
    const Edit edit = number_field(id, number, {.kind = Kind::Slider, .min = min, .max = max, .text = text});
    value = static_cast<float>(number);
    return edit;
}

Edit ivec2(const char* id, glm::ivec2& value, float speed, int min, int max, Unit unit) {
    const Axis axes[] = {{"X", theme::axis_x()}, {"Y", theme::axis_y()}};
    auto number = [](int v) { return value_text("%.0f", v, Unit::None); };
    const std::string before = vector_text(&value.x, 2, unit, number);
    const Id field_id = ui().id(id);
    bool started = false;
    const Edit edit = fields_across(
        id, 2, [&](int i) { return drag_int_axis("##v", value[i], speed, min, max, unit, &axes[i]); }, started);
    detail::track_field(field_id, before, vector_text(&value.x, 2, unit, number), started, edit);
    return edit;
}

Edit vec2(const char* id, glm::vec2& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 2, spec);
}

Edit vec3(const char* id, glm::vec3& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 3, spec);
}

Edit vec4(const char* id, glm::vec4& value, const DragSpec& spec) {
    return vec_n(id, &value.x, 4, spec);
}

InsetFields::InsetFields() {
    kit().frames.push_back({.rest = theme::surface_inset(), .hover = theme::surface_base(), .active = theme::surface_raised()});
}

InsetFields::~InsetFields() {
    kit().frames.pop_back();
}

void readout(const char* text) {
    align_text_to_frame();
    ui::text(text, theme::text_secondary());
}

namespace {

constexpr float TAG_TEXT = 11.5f;
constexpr glm::vec2 TAG_PADDING{6.0f, 1.0f};
constexpr float TAG_ROUNDING = 3.0f;

glm::vec2 tag_size(const char* text) {
    FontScope small(Font::Current, TAG_TEXT);
    return measure(text) + TAG_PADDING * 2.0f;
}

} // namespace

float tag_width(const char* text) {
    return tag_size(text).x;
}

void tag(const char* text, const char* tooltip, const char* detail) {
    const glm::vec2 size = tag_size(text);
    // The item is a whole frame tall, so the chip sits centred on the line
    // of the field before it.
    const float frame = frame_height();
    const core::Rect item = ui().layout().next({size.x, frame});
    place_still(item);
    const glm::vec2 min{item.min.x, item.min.y + std::floor((frame - size.y) * 0.5f)};
    ui().draw_list().rect_filled(min, min + size, faded(theme::surface_raised()), TAG_ROUNDING);
    {
        FontScope small(Font::Current, TAG_TEXT);
        draw_text(min + TAG_PADDING, theme::text_secondary(), text);
    }
    if (tooltip == nullptr) return;
    if (auto tip = ItemTooltip()) {
        ui::text(tooltip);
        if (detail != nullptr) ui::text(detail, theme::text_secondary());
    }
}

Edit on_off(const char* id, bool& on, const char* tooltip) {
    constexpr float W = 28.0f;
    constexpr float H = 16.0f;
    const bool before = on;
    core::Context& context = ui();
    // Centred on the frame line the widgets around it sit on.
    const core::Rect rect = context.layout().next({W, frame_height()});
    const Id switch_id = context.id(id);
    const core::Interaction in = place(switch_id, rect);
    if (in.clicked) on = !on;
    context.semantics(switch_id, core::Role::Toggle, shown(id), {}, {.checked = on});
    if (tooltip != nullptr) item_tooltip(tooltip);
    const float lead = std::floor((rect.height() - H) * 0.5f);
    const glm::vec2 p{rect.min.x, rect.min.y + lead};
    const Color fill = on ? theme::toggle_on() : in.hovered ? theme::surface_highest() : theme::surface_active();
    core::DrawList& dl = context.draw_list();
    dl.rect_filled(p, {p.x + W, p.y + H}, faded(fill), H * 0.5f);
    const float knob = H - 4.0f;
    const float knob_x = on ? p.x + W - 2.0f - knob : p.x + 2.0f;
    dl.circle_filled({knob_x + knob * 0.5f, p.y + H * 0.5f}, knob * 0.5f, faded(on ? theme::text() : theme::text_secondary()));
    const Edit edit{in.clicked, in.clicked};
    track(before ? "On" : "Off", on ? "On" : "Off", edit);
    return edit;
}

Edit stepper(const char* id, int& value, int min, int max, Unit unit) {
    const std::string before = std::to_string(value);
    core::Context& context = ui();
    IdScope scope(id);
    // One frame, the field's colour, holding minus, the number and plus.
    const float h = frame_height();
    constexpr float NUMBER_W = 52.0f;
    const glm::vec2 corner = cursor();
    context.draw_list().rect_filled(corner, corner + glm::vec2{h * 2.0f + NUMBER_W, h}, faded(frame_colours().rest),
                                    kit().style.frame_rounding);
    int next = value;
    {
        Disabled less(value <= min);
        if (icon_button("##less", icon::minus, "Smaller")) next = value - 1;
    }
    same_line(0.0f);
    char number[32];
    std::snprintf(number, sizeof(number), "%d", value);
    const std::string text = detail::with_unit(number, unit_symbol(unit));
    const core::Rect middle = context.layout().next({NUMBER_W, h});
    place_still(middle);
    draw_text({std::floor(middle.min.x + (NUMBER_W - measure(text).x) * 0.5f), middle.min.y + kit().style.frame_padding.y},
              theme::text(), text);
    same_line(0.0f);
    {
        Disabled more(value >= max);
        if (icon_button("##more", icon::add, "Larger")) next = value + 1;
    }
    const bool changed = std::clamp(next, min, max) != value;
    value = std::clamp(next, min, max);
    const Edit edit{changed, changed};
    track(before, std::to_string(value), edit);
    return edit;
}

Edit checkbox(const char* id, bool& value) {
    const char* before = value ? "On" : "Off";
    core::Context& context = ui();
    const Style& style = kit().style;
    const float side = frame_height();
    const std::string_view label = shown(id);
    const float label_w = label.empty() ? 0.0f : style.item_inner_spacing.x + measure(label).x;
    const core::Rect rect = context.layout().next({side + label_w, side});
    const Id box_id = context.id(id);
    const core::Interaction in = place(box_id, rect);
    if (in.clicked) value = !value;
    context.semantics(box_id, core::Role::Toggle, label, {}, {.checked = value});

    const core::Rect box{.min = rect.min, .max = rect.min + glm::vec2{side, side}};
    draw_frame(box, in, false);
    if (value) {
        // ImGui's tick: a short stroke down and a long one up.
        const float pad = std::max(1.0f, std::floor(side / 6.0f));
        const float sz = side - pad * 2.0f;
        const float thickness = std::max(sz / 5.0f, 1.0f);
        const float inner = sz - thickness * 0.5f;
        const glm::vec2 at = box.min + glm::vec2{pad + thickness * 0.25f, pad + thickness * 0.25f};
        const float third = inner / 3.0f;
        const glm::vec2 points[] = {{at.x, at.y + inner - third},
                                    {at.x + third, at.y + inner},
                                    {at.x + third * 3.0f, at.y + inner - third * 3.0f}};
        context.draw_list().polyline(points, faded(corekit::text_colour()), thickness);
    }
    if (!label.empty()) {
        draw_text({box.max.x + style.item_inner_spacing.x, rect.min.y + style.frame_padding.y}, corekit::text_colour(),
                  label);
    }
    const Edit edit{in.clicked, in.clicked};
    track(before, value ? "On" : "Off", edit);
    return edit;
}

} // namespace ftk::ui
