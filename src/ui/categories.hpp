#pragma once

#include "ftk/ui/theme.hpp"

#include <optional>

namespace fjell {

/// What kind of thing something is in the editor, by domain rather than file
/// type: a mesh, its material and its texture are all Rendering, and the icon
/// tells them apart. Each domain has its hue in the theme's palette, which
/// marks it in the hierarchy, the content browser, component bars, node
/// graphs and tags.
enum class Category {
    Rendering, Light, Camera, Environment, Physics, Animation,
    Audio, Vfx, Ui, Logic, Structure,
};

/// The palette's hue for `c`, for the kit's widgets that take one.
[[nodiscard]] constexpr ftk::theme::Hue category_hue(Category c) {
    using H = ftk::theme::Hue;
    switch (c) {
        case Category::Rendering:   return H::blue;
        case Category::Light:       return H::ochre;
        case Category::Camera:      return H::teal;
        case Category::Environment: return H::cyan;
        case Category::Physics:     return H::coral;
        case Category::Animation:   return H::pink;
        case Category::Audio:       return H::orange;
        case Category::Vfx:         return H::periwinkle;
        case Category::Ui:          return H::lilac;
        case Category::Logic:       return H::slate;
        case Category::Structure:   return H::grey;
    }
    return H::grey;
}

/// The hue of `c`, or none for none (a node type that has no category).
[[nodiscard]] constexpr std::optional<ftk::theme::Hue> category_hue(std::optional<Category> c) {
    return c ? std::optional{category_hue(*c)} : std::nullopt;
}

/// The colour of `c`'s hue.
[[nodiscard]] inline ImVec4 category_colour(Category c) {
    return ftk::theme::hue(category_hue(c));
}

/// What a category is called where it names a group (the Add Component
/// picker).
[[nodiscard]] constexpr const char* category_name(Category c) {
    switch (c) {
        case Category::Rendering:   return "Rendering";
        case Category::Light:       return "Light";
        case Category::Camera:      return "Camera";
        case Category::Environment: return "Environment";
        case Category::Physics:     return "Physics";
        case Category::Animation:   return "Animation";
        case Category::Audio:       return "Audio";
        case Category::Vfx:         return "VFX";
        case Category::Ui:          return "UI";
        case Category::Logic:       return "Logic";
        case Category::Structure:   return "Structure";
    }
    return "Structure";
}

} // namespace fjell
