#pragma once

// Scopes over what is drawn while they live.
//
//     {
//         ui::Disabled off(!can_bake);
//         if (ui::button("Bake")) bake();
//     }
//
//     for (const Layer& layer : layers) {
//         ui::IdScope id(layer.name);
//         ui::row("Opacity", ...);  // the same label in every layer, told apart
//     }

#include <string_view>

namespace ftk::ui {

/// While it lives, what is drawn is faded and takes no input, when
/// `disabled`. Inside a disabled scope everything stays disabled.
class Disabled {
public:
    explicit Disabled(bool disabled = true);
    ~Disabled();
    Disabled(const Disabled&) = delete;
    Disabled& operator=(const Disabled&) = delete;
    Disabled(Disabled&&) = delete;
    Disabled& operator=(Disabled&&) = delete;
};

/// While it lives, widgets are told apart by `id` as well as by their
/// labels: the same label once per element of a list.
class IdScope {
public:
    explicit IdScope(std::string_view id);
    explicit IdScope(const char* id);
    explicit IdScope(int id);
    explicit IdScope(const void* id);
    ~IdScope();
    IdScope(const IdScope&) = delete;
    IdScope& operator=(const IdScope&) = delete;
    IdScope(IdScope&&) = delete;
    IdScope& operator=(IdScope&&) = delete;
};

} // namespace ftk::ui
