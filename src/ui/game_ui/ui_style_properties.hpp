#pragma once

#include "ui/game_ui/ui_style.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace fjell {

/// What kind of value a CSS property carries. The kind decides how the
/// value is parsed, which inspector widget edits it and how a transition
/// interpolates it.
enum class UiPropertyKind : uint8_t {
    number,    ///< a float member: pixels or a unitless factor
    length,    ///< a UiLength member: pixels, a percentage or auto
    color,     ///< a UiColor member
    edges,     ///< a UiEdges member set as a whole (`margin: 1 2 3 4`)
    edge,      ///< one side of a UiEdges member (`margin-top`)
    corners,   ///< a UiCorners member (`border-radius`)
    keyword,   ///< an enum member chosen from a fixed keyword list
    composite, ///< bespoke syntax (`border`, `background`, `box-shadow`, ...)
    string,    ///< a std::string member, quotes stripped (`font-family`)
};

enum class UiEdgeSide : uint8_t { top, right, bottom, left };

[[nodiscard]] UiLength& ui_edge(UiEdges& edges, UiEdgeSide side);
[[nodiscard]] const UiLength& ui_edge(const UiEdges& edges, UiEdgeSide side);

/// The UiComputedStyle member a property writes. monostate for keywords,
/// which reach their members through their own functions, and for
/// composites spread over several members. A composite that writes one
/// member names it, so it can be inherited like any other property.
using UiStyleMember = std::variant<std::monostate,
                                   float UiComputedStyle::*,
                                   UiLength UiComputedStyle::*,
                                   UiColor UiComputedStyle::*,
                                   UiEdges UiComputedStyle::*,
                                   UiCorners UiComputedStyle::*,
                                   std::string UiComputedStyle::*,
                                   UiTextShadow UiComputedStyle::*>;

/// One row of the property table: the authored name, the member it
/// targets and what the cascade, the transition system and the editor
/// inspector each need to know about it. All three read this one table
/// instead of keeping their own list of property names.
struct UiPropertyDescriptor {
    std::string_view name;
    UiPropertyKind kind{UiPropertyKind::number};
    UiStyleMember member{};
    UiEdgeSide side{UiEdgeSide::top}; ///< which side, for kind == edge

    /// Accepted spellings for kind == keyword, in the target enum's
    /// declaration order so an index converts straight to the enum. Every
    /// entry is a string literal, so `data()` is null-terminated for ImGui.
    std::span<const std::string_view> keywords{};
    void (*set_keyword)(UiComputedStyle&, uint8_t index){nullptr};
    uint8_t (*get_keyword)(const UiComputedStyle&){nullptr};

    bool animatable{false}; ///< a `transition:` may interpolate it
    bool inherited{false};  ///< passes from parent to child when not set
    /// For lengths: `auto` is an accepted value. Width, height, margin and
    /// the insets take it; the bounds, padding and gaps do not.
    bool auto_allowed{false};
};

/// Every property the cascade understands, in a stable order suitable for
/// listing in an inspector.
[[nodiscard]] std::span<const UiPropertyDescriptor> ui_style_properties();

/// The descriptor for an authored name, or nullptr when the engine has no
/// such property — which the parser reports rather than dropping silently.
[[nodiscard]] const UiPropertyDescriptor* find_ui_style_property(std::string_view name);

/// A font weight as CSS writes it: a number from 1 to 1000, `normal` (400)
/// or `bold` (700). Nothing for anything else.
[[nodiscard]] std::optional<float> parse_ui_font_weight(std::string_view value);

/// The text of a string property with surrounding quotes removed.
[[nodiscard]] std::string ui_style_unquote(std::string_view value);

/// A composite value (`border`, `box-shadow`, `text-shadow`) split on
/// whitespace, with an rgb()/rgba() colour kept whole although it contains
/// spaces.
[[nodiscard]] std::vector<std::string> ui_style_value_tokens(std::string_view value);

/// Whether a token of such a value is a length rather than a colour or a
/// keyword: it starts like a number (`2`, `-1.5`, `.5px`).
[[nodiscard]] bool ui_style_is_length_token(std::string_view token);

/// Scalar channels a transition interpolates for the property: one for a
/// number or edge, four for a color or corners, none when not animatable.
[[nodiscard]] uint8_t ui_style_channel_count(const UiPropertyDescriptor& desc);
[[nodiscard]] float ui_style_read_channel(const UiComputedStyle& style,
                                          const UiPropertyDescriptor& desc, uint8_t channel);
void ui_style_write_channel(UiComputedStyle& style, const UiPropertyDescriptor& desc,
                            uint8_t channel, float value);

/// Whether a transition can interpolate the property between these two
/// styles. Lengths of different units, or auto on either side, have no
/// meaningful in-between and snap to the new value instead.
[[nodiscard]] bool ui_style_channel_interpolable(const UiComputedStyle& from,
                                                 const UiComputedStyle& to,
                                                 const UiPropertyDescriptor& desc);

/// Copies the property's value from `from` into `into` — inheritance.
void ui_style_copy(UiComputedStyle& into, const UiComputedStyle& from,
                   const UiPropertyDescriptor& desc);

} // namespace fjell
