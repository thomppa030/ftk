#pragma once

// What each widget is, recorded as it is drawn (decision 10 of the UI core
// plan): its role, its label, its value and its state. Tests find widgets
// by name with it (h.click("Save")), scripts and agents read and drive the
// UI with it, and a screen reader adapter can come later over the same
// records.
//
//     ui.semantics(id, Role::Slider, "Opacity", "0.75");

#include "ftk/ui/core/input.hpp"
#include "ftk/ui/core/rect.hpp"
#include "ftk/ui/core/types.hpp"

#include <cstdint>
#include <string_view>

namespace ftk::ui::core {

enum class Role : uint8_t {
    None,
    Button,
    /// On or off: a checkbox, a switch, a toggle button.
    Toggle,
    /// A number dragged or typed: a slider, a number field.
    Slider,
    TextField,
    /// One of several, from a list that opens.
    Choice,
    MenuItem,
    ListItem,
    TreeItem,
    Tab,
    /// Text shown, not taking input.
    Text,
    Image,
    Canvas,
    Panel,
    Popup,
    Dialog,
    Tooltip,
};

struct SemanticState {
    bool disabled{false};
    bool focused{false};
    bool checked{false};
    bool selected{false};
    bool expanded{false};
};

/// One widget as the frame recorded it.
struct SemanticNode {
    Id id{0};
    Role role{Role::None};
    std::string_view label{};
    std::string_view value{};
    SemanticState state{};
    /// Where it is, in its window.
    Rect rect{};
    RootId root{0};
};

} // namespace ftk::ui::core
