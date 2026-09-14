#pragma once

#include "ui/game_ui/ui_style.hpp"

#include <cstdint>
#include <span>
#include <string_view>
#include <variant>

namespace fjell {

/// What kind of value a CSS property carries. The kind decides how the
/// value is parsed, which inspector widget edits it and how a transition
/// interpolates it.
enum class UiPropertyKind : uint8_t {
    number,    ///< a float member: pixels or a unitless factor
    color,     ///< a UiColor member
    edges,     ///< a UiEdges member set as a whole (`margin: 1 2 3 4`)
    edge,      ///< one side of a UiEdges member (`margin-top`)
    corners,   ///< a UiCorners member (`border-radius`)
    keyword,   ///< an enum member chosen from a fixed keyword list
    composite, ///< bespoke syntax (`border`, `background`, `box-shadow`, ...)
};

enum class UiEdgeSide : uint8_t { top, right, bottom, left };

[[nodiscard]] float& ui_edge(UiEdges& edges, UiEdgeSide side);
[[nodiscard]] const float& ui_edge(const UiEdges& edges, UiEdgeSide side);

/// The UiComputedStyle member a property writes. monostate for composites
/// and keywords, which reach their members through their own functions.
using UiStyleMember = std::variant<std::monostate,
                                   float UiComputedStyle::*,
                                   UiColor UiComputedStyle::*,
                                   UiEdges UiComputedStyle::*,
                                   UiCorners UiComputedStyle::*>;

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
};

/// Every property the cascade understands, in a stable order suitable for
/// listing in an inspector.
[[nodiscard]] std::span<const UiPropertyDescriptor> ui_style_properties();

/// The descriptor for an authored name, or nullptr when the engine has no
/// such property — which the parser reports rather than dropping silently.
[[nodiscard]] const UiPropertyDescriptor* find_ui_style_property(std::string_view name);

/// Scalar channels a transition interpolates for the property: one for a
/// number or edge, four for a color or corners, none when not animatable.
[[nodiscard]] uint8_t ui_style_channel_count(const UiPropertyDescriptor& desc);
[[nodiscard]] float ui_style_read_channel(const UiComputedStyle& style,
                                          const UiPropertyDescriptor& desc, uint8_t channel);
void ui_style_write_channel(UiComputedStyle& style, const UiPropertyDescriptor& desc,
                            uint8_t channel, float value);

/// Copies the property's value from `from` into `into` — inheritance.
void ui_style_copy(UiComputedStyle& into, const UiComputedStyle& from,
                   const UiPropertyDescriptor& desc);

} // namespace fjell
