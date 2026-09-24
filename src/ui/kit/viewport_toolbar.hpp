#pragma once

#include <imgui.h>

// The toolbar of a viewport (sheet 28): small dark pills floating over the
// picture's top corners, each holding kit buttons laid out with SameLine.
// What changes the view goes here, never what changes the asset.
//
//     if (auto pill = ui::ViewportPill("##view", image_min, image_max, ui::PillPlace::TopRight)) {
//         ui::mode_button(...); ImGui::SameLine(); ui::icon_button(...);
//     }

namespace fjell::ui {

enum class PillPlace { TopLeft, TopCenter, TopRight };

class ViewportPill {
public:
    /// A pill over the image spanning `image_min` to `image_max`.
    ViewportPill(const char* id, ImVec2 image_min, ImVec2 image_max, PillPlace place);
    ~ViewportPill();
    ViewportPill(const ViewportPill&) = delete;
    ViewportPill& operator=(const ViewportPill&) = delete;
    ViewportPill(ViewportPill&&) = delete;
    ViewportPill& operator=(ViewportPill&&) = delete;

    explicit operator bool() const { return true; }

private:
    ImGuiID size_id_{0};
    ImVec2 min_{};
};

} // namespace fjell::ui

namespace fjell::ui {

/// A window that shows a picture edge to edge, a viewport: no padding and
/// no background of its own, so the image is all there is. Ends the window
/// when it goes out of scope, whether or not it was visible.
class ViewportWindow {
public:
    ViewportWindow(const char* title, bool* open = nullptr, ImGuiWindowFlags flags = 0);
    ~ViewportWindow();
    ViewportWindow(const ViewportWindow&) = delete;
    ViewportWindow& operator=(const ViewportWindow&) = delete;
    ViewportWindow(ViewportWindow&&) = delete;
    ViewportWindow& operator=(ViewportWindow&&) = delete;

    explicit operator bool() const { return visible_; }

private:
    bool visible_{false};
};

} // namespace fjell::ui
