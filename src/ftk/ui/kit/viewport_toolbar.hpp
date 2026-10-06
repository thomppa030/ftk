#pragma once

#include "ftk/ui/kit/window.hpp"
#include "ftk/ui/types.hpp"

#include <glm/vec2.hpp>

// The toolbar of a viewport (sheet 28): small dark pills floating over the
// picture's top corners, each holding kit buttons laid out with same_line.
// What changes the view goes here, never what changes the asset. A pill in
// the bottom-right corner is a plate that grows to what it holds, such as a
// texture's mips.
//
//     if (auto pill = ui::ViewportPill("##view", image_min, image_max, ui::PillPlace::TopRight)) {
//         ui::mode_button(...); ui::same_line(); ui::icon_button(...);
//     }

namespace ftk::ui {

enum class PillPlace { TopLeft, TopCenter, TopRight, BottomRight };

class ViewportPill {
public:
    /// A pill over the image spanning `image_min` to `image_max`.
    ViewportPill(const char* id, glm::vec2 image_min, glm::vec2 image_max, PillPlace place);
    ~ViewportPill();
    ViewportPill(const ViewportPill&) = delete;
    ViewportPill& operator=(const ViewportPill&) = delete;
    ViewportPill(ViewportPill&&) = delete;
    ViewportPill& operator=(ViewportPill&&) = delete;

    explicit operator bool() const { return true; }

private:
    Id size_id_{0};
    Id height_id_{0};
    glm::vec2 min_{};
};

/// Where the pills along the top of an image end, for what is placed
/// under them.
[[nodiscard]] float pill_row_bottom(glm::vec2 image_min);

} // namespace ftk::ui

namespace ftk::ui {

/// A window that shows a picture edge to edge, a viewport: no padding and
/// no background of its own, so the image is all there is. Ends the window
/// when it goes out of scope, whether or not it was visible.
/// Begins a window that fills its place edge to edge, with no rounding,
/// border or padding: the host of a dockspace. It has no title bar and is
/// never resized, moved or collapsed, whatever `options` say. Its style is
/// its own, so nothing drawn in it inherits it. End it with
/// end_host_window(), whether or not it is visible.
bool begin_host_window(const char* name, WindowOptions options = {});

/// Ends the window begin_host_window() began.
void end_host_window();

class ViewportWindow {
public:
    ViewportWindow(const char* title, bool* open = nullptr, WindowOptions options = {});
    ~ViewportWindow();
    ViewportWindow(const ViewportWindow&) = delete;
    ViewportWindow& operator=(const ViewportWindow&) = delete;
    ViewportWindow(ViewportWindow&&) = delete;
    ViewportWindow& operator=(ViewportWindow&&) = delete;

    explicit operator bool() const { return visible_; }

private:
    bool visible_{false};
};

} // namespace ftk::ui
