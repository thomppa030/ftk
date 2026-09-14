#include "ui/game_ui/ui_style_properties.hpp"

#include <type_traits>

namespace fjell {

float& ui_edge(UiEdges& edges, UiEdgeSide side) {
    switch (side) {
    case UiEdgeSide::top: return edges.top;
    case UiEdgeSide::right: return edges.right;
    case UiEdgeSide::bottom: return edges.bottom;
    case UiEdgeSide::left: return edges.left;
    }
    return edges.top;
}

const float& ui_edge(const UiEdges& edges, UiEdgeSide side) {
    switch (side) {
    case UiEdgeSide::top: return edges.top;
    case UiEdgeSide::right: return edges.right;
    case UiEdgeSide::bottom: return edges.bottom;
    case UiEdgeSide::left: return edges.left;
    }
    return edges.top;
}

namespace {

// Keyword properties all follow the same shape — an enum member written
// from an index into its keyword list — so one pair of templates covers
// every enum without a hand-written setter per type.
template <auto Member>
void set_enum(UiComputedStyle& style, uint8_t index) {
    using Enum = std::remove_cvref_t<decltype(style.*Member)>;
    style.*Member = static_cast<Enum>(index);
}

template <auto Member>
uint8_t get_enum(const UiComputedStyle& style) {
    return static_cast<uint8_t>(style.*Member);
}

// Each list is in its enum's declaration order (see ui_types.hpp).
constexpr std::string_view DISPLAY_KEYWORDS[] = {"flex", "none"};
constexpr std::string_view FLEX_DIRECTION_KEYWORDS[] = {
    "row", "column", "row-reverse", "column-reverse"};
constexpr std::string_view JUSTIFY_KEYWORDS[] = {
    "flex-start", "flex-end", "center", "space-between", "space-around", "space-evenly"};
constexpr std::string_view ALIGN_KEYWORDS[] = {
    "auto", "flex-start", "flex-end", "center", "stretch", "baseline"};
constexpr std::string_view FLEX_WRAP_KEYWORDS[] = {"nowrap", "wrap", "wrap-reverse"};
constexpr std::string_view POSITION_KEYWORDS[] = {"relative", "absolute"};
constexpr std::string_view OVERFLOW_KEYWORDS[] = {"visible", "hidden", "scroll"};

using Kind = UiPropertyKind;
using Side = UiEdgeSide;
using Style = UiComputedStyle;

constexpr UiPropertyDescriptor keyword(std::string_view name,
                                       std::span<const std::string_view> keywords,
                                       void (*set)(Style&, uint8_t),
                                       uint8_t (*get)(const Style&)) {
    return {.name = name, .kind = Kind::keyword, .keywords = keywords,
            .set_keyword = set, .get_keyword = get};
}

constexpr UiPropertyDescriptor number(std::string_view name, float Style::* member,
                                      bool animatable = true, bool inherited = false) {
    return {.name = name, .kind = Kind::number, .member = member,
            .animatable = animatable, .inherited = inherited};
}

constexpr UiPropertyDescriptor color(std::string_view name, UiColor Style::* member,
                                     bool inherited = false) {
    return {.name = name, .kind = Kind::color, .member = member,
            .animatable = true, .inherited = inherited};
}

constexpr UiPropertyDescriptor edges(std::string_view name, UiEdges Style::* member) {
    return {.name = name, .kind = Kind::edges, .member = member};
}

constexpr UiPropertyDescriptor edge(std::string_view name, UiEdges Style::* member, Side side) {
    return {.name = name, .kind = Kind::edge, .member = member, .side = side, .animatable = true};
}

constexpr UiPropertyDescriptor composite(std::string_view name) {
    return {.name = name, .kind = Kind::composite};
}

const UiPropertyDescriptor PROPERTIES[] = {
    // Layout
    keyword("display", DISPLAY_KEYWORDS,
            set_enum<&Style::display>, get_enum<&Style::display>),
    keyword("flex-direction", FLEX_DIRECTION_KEYWORDS,
            set_enum<&Style::flex_direction>, get_enum<&Style::flex_direction>),
    keyword("justify-content", JUSTIFY_KEYWORDS,
            set_enum<&Style::justify_content>, get_enum<&Style::justify_content>),
    keyword("align-items", ALIGN_KEYWORDS,
            set_enum<&Style::align_items>, get_enum<&Style::align_items>),
    keyword("align-self", ALIGN_KEYWORDS,
            set_enum<&Style::align_self>, get_enum<&Style::align_self>),
    keyword("flex-wrap", FLEX_WRAP_KEYWORDS,
            set_enum<&Style::flex_wrap>, get_enum<&Style::flex_wrap>),
    keyword("position", POSITION_KEYWORDS,
            set_enum<&Style::position>, get_enum<&Style::position>),
    keyword("overflow", OVERFLOW_KEYWORDS,
            set_enum<&Style::overflow>, get_enum<&Style::overflow>),
    number("flex-grow", &Style::flex_grow),
    number("flex-shrink", &Style::flex_shrink),

    // Dimensions
    number("width", &Style::width),
    number("height", &Style::height),
    number("min-width", &Style::min_width),
    number("min-height", &Style::min_height),
    number("max-width", &Style::max_width),
    number("max-height", &Style::max_height),

    // Spacing
    edges("margin", &Style::margin),
    edge("margin-top", &Style::margin, Side::top),
    edge("margin-right", &Style::margin, Side::right),
    edge("margin-bottom", &Style::margin, Side::bottom),
    edge("margin-left", &Style::margin, Side::left),
    edges("padding", &Style::padding),
    edge("padding-top", &Style::padding, Side::top),
    edge("padding-right", &Style::padding, Side::right),
    edge("padding-bottom", &Style::padding, Side::bottom),
    edge("padding-left", &Style::padding, Side::left),
    edge("top", &Style::inset, Side::top),
    edge("right", &Style::inset, Side::right),
    edge("bottom", &Style::inset, Side::bottom),
    edge("left", &Style::inset, Side::left),
    composite("gap"),
    number("row-gap", &Style::row_gap),
    number("column-gap", &Style::column_gap),

    // Visual
    composite("background"),
    color("background-color", &Style::background_color),
    composite("border"),
    color("border-color", &Style::border_color),
    number("border-width", &Style::border_width),
    {.name = "border-radius", .kind = Kind::corners, .member = &Style::border_radius,
     .animatable = true},
    number("opacity", &Style::opacity, true, true),
    composite("box-shadow"),

    // Text
    color("color", &Style::color, true),
    number("font-size", &Style::font_size, true, true),
    number("line-height", &Style::line_height, true, true),

    composite("transition"),
};

// Templated on the style's constness so reads and writes share one
// mapping from (property, channel) to the float it lives in.
template <typename Color>
auto& color_channel(Color& c, uint8_t channel) {
    switch (channel) {
    case 0: return c.r;
    case 1: return c.g;
    case 2: return c.b;
    default: return c.a;
    }
}

template <typename Corners>
auto& corner_channel(Corners& c, uint8_t channel) {
    switch (channel) {
    case 0: return c.top_left;
    case 1: return c.top_right;
    case 2: return c.bottom_right;
    default: return c.bottom_left;
    }
}

template <typename StyleRef>
auto& channel_ref(StyleRef& style, const UiPropertyDescriptor& desc, uint8_t channel) {
    switch (desc.kind) {
    case Kind::number:
        return style.*std::get<float Style::*>(desc.member);
    case Kind::edge:
        return ui_edge(style.*std::get<UiEdges Style::*>(desc.member), desc.side);
    case Kind::color:
        return color_channel(style.*std::get<UiColor Style::*>(desc.member), channel);
    case Kind::corners:
        return corner_channel(style.*std::get<UiCorners Style::*>(desc.member), channel);
    case Kind::edges:
    case Kind::keyword:
    case Kind::composite:
        break;
    }
    return style.opacity; // unreachable for animatable kinds; see ui_style_channel_count
}

} // namespace

std::span<const UiPropertyDescriptor> ui_style_properties() {
    return PROPERTIES;
}

const UiPropertyDescriptor* find_ui_style_property(std::string_view name) {
    for (const auto& desc : PROPERTIES) {
        if (desc.name == name) return &desc;
    }
    return nullptr;
}

uint8_t ui_style_channel_count(const UiPropertyDescriptor& desc) {
    if (!desc.animatable) return 0;
    switch (desc.kind) {
    case Kind::number:
    case Kind::edge:
        return 1;
    case Kind::color:
    case Kind::corners:
        return 4;
    case Kind::edges:
    case Kind::keyword:
    case Kind::composite:
        return 0;
    }
    return 0;
}

float ui_style_read_channel(const UiComputedStyle& style, const UiPropertyDescriptor& desc,
                            uint8_t channel) {
    return channel_ref(style, desc, channel);
}

void ui_style_write_channel(UiComputedStyle& style, const UiPropertyDescriptor& desc,
                            uint8_t channel, float value) {
    channel_ref(style, desc, channel) = value;
}

void ui_style_copy(UiComputedStyle& into, const UiComputedStyle& from,
                   const UiPropertyDescriptor& desc) {
    switch (desc.kind) {
    case Kind::keyword:
        desc.set_keyword(into, desc.get_keyword(from));
        return;
    case Kind::composite:
        return;
    case Kind::number:
    case Kind::color:
    case Kind::edges:
    case Kind::edge:
    case Kind::corners:
        break;
    }
    std::visit([&](auto member) {
        if constexpr (!std::is_same_v<decltype(member), std::monostate>) {
            into.*member = from.*member;
        }
    }, desc.member);
}

} // namespace fjell
