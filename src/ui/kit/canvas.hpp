#pragma once

#include <glm/common.hpp>
#include <glm/vec2.hpp>
#include <imgui.h>

#include <limits>
#include <string>

// What every editor canvas shares (sheet 8): how the world maps to the
// screen, and how the mouse moves that mapping. The wheel zooms at the
// cursor, a middle drag (or Alt and a left drag) pans, and F frames what
// the canvas holds. A canvas draws and hit-tests its own content through
// its view, with one hit radius, and marks handles with the kit's handle.
//
//     view_.place(origin, size);
//     ImGui::InvisibleButton("##canvas", size, ...);
//     view_.input(ImGui::IsItemHovered());
//     if (!view_.panning() && ImGui::IsItemActivated()) { ...pick what is under the mouse... }
//     const ImVec2 at = view_.to_screen(point);

namespace fjell::ui {

/// How near the mouse must come to a handle, a badge or a line to take it,
/// in screen pixels. The same on every canvas.
inline constexpr float CANVAS_HIT_RADIUS = 8.0f;

enum class CanvasZoom {
    /// Both axes by one scale, kept when the canvas is resized: a graph.
    Uniform,
    /// Across the width only, the span kept when it is resized: a timeline.
    Horizontal,
    /// Each axis on its own, what is in view kept when it is resized: a
    /// plot of two values. Shift and the wheel zoom across only.
    Both,
};

/// The part of the world a canvas shows. Belongs to the canvas instance;
/// with recall() it is also kept for the session under an asset's key, so
/// the asset reopens where it was left.
class CanvasView {
public:
    struct Options {
        CanvasZoom zoom{CanvasZoom::Uniform};
        /// Uniform: the scale's limits, in screen pixels per world unit.
        float min_scale{0.3f};
        float max_scale{2.5f};
        /// Horizontal: the world the view never leaves, and the least of it
        /// the width shows.
        float bound_min{-std::numeric_limits<float>::infinity()};
        float bound_max{std::numeric_limits<float>::infinity()};
        float min_span{1e-3f};
        /// Both: values grow upward, the way a plot reads.
        bool y_up{false};
    };

    CanvasView() = default;
    explicit CanvasView(Options options) : options_{options} {}

    /// Where the canvas is on screen this frame. Call before mapping or
    /// input; calling it again in the same frame for another part of the
    /// canvas (a ruler over lanes) moves the mapping, not the view.
    void place(ImVec2 origin, ImVec2 size);

    /// The wheel and a pan starting, over the item just drawn. Call after
    /// each item that covers part of the canvas.
    void input(bool hovered);
    /// True while the mouse pans the view: the click that started it is
    /// not the canvas's to handle.
    [[nodiscard]] bool panning() const { return panning_; }

    /// Shows the world from `lo` to `hi`. Uniform fits it in the canvas
    /// without enlarging past one pixel per unit; Horizontal shows lo.x to
    /// hi.x within its bounds; Both shows exactly that rectangle.
    void frame(glm::vec2 lo, glm::vec2 hi);

    /// Horizontal: moves the bounds (a clip got longer). A view showing all
    /// of the old bounds goes on showing all of the new ones.
    void set_bounds(float lo, float hi);

    /// Keeps the view under `key` for the session and brings back what was
    /// kept there before. False when nothing was: the caller frames.
    bool recall(const std::string& key);

    /// For a canvas drawn by a function rather than kept by an object (a
    /// curve in an inspector): the view ImGui keeps under `id` in the
    /// current window, which is per asset since every editor's windows are.
    /// False when there is none yet: the caller frames.
    bool load(ImGuiID id);
    /// Keeps the view, and where it was last placed, under `id` for the
    /// next frame's load().
    void store(ImGuiID id) const;

    [[nodiscard]] ImVec2 to_screen(glm::vec2 world) const;
    [[nodiscard]] glm::vec2 to_world(ImVec2 screen) const;
    [[nodiscard]] float to_screen_x(float world) const;
    [[nodiscard]] float to_world_x(float screen) const;
    /// Screen pixels per world unit across and down.
    [[nodiscard]] float scale_x() const;
    [[nodiscard]] float scale_y() const;
    [[nodiscard]] ImVec2 origin() const { return origin_; }
    [[nodiscard]] ImVec2 size() const { return size_; }

private:
    void clamp_horizontal();
    /// Writes the view where recall() finds it, after every change.
    void keep();

    Options options_{};
    ImVec2 origin_{};
    ImVec2 size_{1.0f, 1.0f};
    glm::vec2 min_{0.0f};   // the world at the canvas's top-left
    float scale_{1.0f};     // Uniform
    glm::vec2 span_{1.0f};  // Horizontal and Both: the world across the canvas
    bool panning_{false};
    int panned_frame_{-1};
    std::string key_;
};

/// True when the canvas's window has the keyboard and nothing else is being
/// edited, so F and Del are the canvas's.
[[nodiscard]] bool canvas_has_keys();

/// Lines every `step` world units across the visible canvas, both ways,
/// on whole pixels; none when they would come closer than `min_px`.
void canvas_grid(ImDrawList* dl, const CanvasView& view, float step, const ImVec4& colour, float min_px = 6.0f);

/// A plot's axes (sheet 8): lines at round values in the plot, the major
/// ones brighter and zero brighter still, and their values in gutters
/// outside it, each axis named beside its values. Without `y_axis` (one
/// value along a line) there is no left gutter and no lines across.
struct PlotAxes {
    const char* x_name{nullptr};
    const char* y_name{nullptr};
    bool y_axis{true};
};

/// Where the plot sits in `origin`..`origin + size` once its gutters are
/// taken: place the plot's view there.
void plot_area(ImVec2 origin, ImVec2 size, const PlotAxes& axes, ImVec2* plot_origin, ImVec2* plot_size);

/// Draws the plot (the view's area) with its grid, and the gutters around it.
void canvas_plot_axes(ImDrawList* dl, const CanvasView& view, const PlotAxes& axes);

/// A time ruler across the top of a Horizontal canvas, `height` tall:
/// ticks at round times, the major ones valued.
void canvas_ruler(ImDrawList* dl, const CanvasView& view, float height);

/// The playhead at screen `x` from `top` to `bottom`, headed by a small
/// triangle when it runs through a ruler.
void canvas_playhead(ImDrawList* dl, float x, float top, float bottom, bool head);

enum class HandleShape { Circle, Diamond };

struct HandleLook {
    HandleShape shape{HandleShape::Circle};
    float radius{4.0f};
    /// The handle's own colour (a gradient stop's): the state then shows
    /// in its outline.
    const ImVec4* fill{nullptr};
};

/// A point the mouse can take: at rest in the secondary text colour,
/// brighter with a ring under the mouse, the accent when selected.
void canvas_handle(ImDrawList* dl, ImVec2 at, bool hot, bool selected, HandleLook look = {});

/// What a handle being dragged is at, in a small box beside it, kept inside
/// `clip_min`..`clip_max`.
void canvas_readout(ImDrawList* dl, ImVec2 handle, const char* text, ImVec2 clip_min, ImVec2 clip_max);

} // namespace fjell::ui
