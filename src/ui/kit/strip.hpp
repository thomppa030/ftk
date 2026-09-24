#pragma once

#include <imgui.h>

// A row of an editor's own settings on a raised strip across its panel, above
// what the panel lists (an input context's name and switches, sheet 16).
//
//     if (auto strip = ui::RaisedStrip("##context")) {
//         // fields on one line, SameLine between them
//     }

namespace fjell::ui {

class RaisedStrip {
public:
    explicit RaisedStrip(const char* id);
    ~RaisedStrip();
    RaisedStrip(const RaisedStrip&) = delete;
    RaisedStrip& operator=(const RaisedStrip&) = delete;
    RaisedStrip(RaisedStrip&&) = delete;
    RaisedStrip& operator=(RaisedStrip&&) = delete;

    explicit operator bool() const { return true; }

private:
    ImGuiID height_id_{0};
    ImVec2 min_{};
    float width_{0.0f};
};

} // namespace fjell::ui
