#pragma once

#include <string_view>

// A key, mouse button or pad input as a cap sized to its content (sheet 16):
// a raised key with a darker lip (sheet 26), the device's icon then the key's name
// in the bold face, and at its right end a chevron set off by a divider.
// Clicking the name listens for a new press, with an amber edge and a note
// on how to stop; the chevron opens a list to pick from, for what can't be
// pressed here (an axis, a pad that isn't plugged in).

namespace fjell::ui {

struct KeyCapSpec {
    const char* id{""};
    /// ui::icon::keyboard, mouse or gamepad; null leaves it out (a key in a
    /// four-key cluster).
    const char* device_icon{nullptr};
    /// The key's name; empty for nothing bound.
    std::string_view label{};
    bool listening{false};
    /// Narrower, for the four caps of a W / A S D cluster.
    bool compact{false};
};

struct KeyCapResult {
    /// The name was clicked: listen for a press.
    bool listen{false};
    /// The chevron was clicked: open the list. The last item is then the
    /// chevron, for a popup to hang from.
    bool list{false};
};

KeyCapResult key_cap(const KeyCapSpec& spec);

} // namespace fjell::ui
