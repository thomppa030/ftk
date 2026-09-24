#pragma once

#include <imgui.h>

// A part of a standalone window, such as the hub's sidebar (sheet 9): filled
// edge to edge in its surface, with the kit's window padding inside.
//
//     { ui::Pane side("##side", {220.0f, height}, ui::PaneSurface::Sunken); ... }
//     ImGui::SameLine(0.0f, 0.0f);
//     { ui::Pane main("##main", {0.0f, height}); ... }

namespace fjell::ui {

enum class PaneSurface { Base, Sunken };

class Pane {
public:
    Pane(const char* id, ImVec2 size, PaneSurface surface = PaneSurface::Base);
    ~Pane();
    Pane(const Pane&) = delete;
    Pane& operator=(const Pane&) = delete;
    Pane(Pane&&) = delete;
    Pane& operator=(Pane&&) = delete;

    explicit operator bool() const { return visible_; }

private:
    bool visible_{false};
};

} // namespace fjell::ui
